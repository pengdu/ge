// VideoFilter@1.0.0: an arbitrary libavfilter chain between two VideoFrame
// ports. Built for the "insert / remove a filter mid-stream" scene (03
// §5.3 TR-U-6, MUT-1a): the operator is format-neutral (its output pixel
// format equals the negotiated contract, a trailing format= pins it) so it
// can be InsertChain'ed on any decoded-video edge, typically scale -> venc,
// and RemoveChain'ed with bypass without touching the neighbours.
//
// Options: {"filter": "drawtext=text='hi':x=10:y=10:fontsize=24:fontcolor=white"}
// The filter string is hot-updatable: a parameter change rebuilds the graph
// at the next packet boundary after draining the old one (same pattern as
// VideoScale's watermark).
#include <sstream>

#include "media/operators_internal.h"

namespace ge::media {

namespace {

// Whatever libavfilter reports for a chain that does not parse or
// configure (unknown filter -> ENOENT, unknown option -> AVERROR_OPTION_NOT_FOUND,
// bad value -> EINVAL) is an invalid "filter" option from the host's point
// of view; only allocation failures keep their own code.
Status ChainError(int err, const std::string& what) {
  const Status s = ff::ToStatus(err, what);
  if (s.code() == GE_STATUS_RESOURCE_EXHAUSTED) return s;
  return Status::InvalidArgument(s.message());
}

Status LinkChain(AVFilterGraph* g, AVFilterContext* src, AVFilterContext* sink, const std::string& chain,
                 const std::string& out_pixel, const std::string& prefix) {
  const std::string full = chain + ",format=pix_fmts=" + out_pixel;
  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();
  if (outputs == nullptr || inputs == nullptr) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return Status::ResourceExhausted(prefix + "filter inout alloc");
  }
  outputs->name = av_strdup("in");
  outputs->filter_ctx = src;
  outputs->pad_idx = 0;
  outputs->next = nullptr;
  inputs->name = av_strdup("out");
  inputs->filter_ctx = sink;
  inputs->pad_idx = 0;
  inputs->next = nullptr;
  int err = avfilter_graph_parse_ptr(g, full.c_str(), &inputs, &outputs, nullptr);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (err < 0) return ChainError(err, prefix + "parse '" + chain + "'");
  err = avfilter_graph_config(g, nullptr);
  if (err < 0) return ChainError(err, prefix + "graph config '" + chain + "'");
  return Status::Ok();
}

// buffer(w x h, fmt) -> <chain> -> format=out_pixel -> buffersink.
Status BuildChain(ff::FilterGraphPtr* graph, AVFilterContext** src, AVFilterContext** sink, int w, int h, int fmt,
                  AVRational sar, const std::string& chain, const std::string& out_pixel, const std::string& prefix) {
  if (chain.empty()) return Status::InvalidArgument(prefix + "filter is required");
  graph->reset(avfilter_graph_alloc());
  if (!*graph) return Status::ResourceExhausted(prefix + "filter graph alloc");
  std::ostringstream args;
  args << "video_size=" << w << "x" << h << ":pix_fmt=" << fmt << ":time_base=1/1000000000:pixel_aspect=" << sar.num
       << "/" << sar.den;
  int err = avfilter_graph_create_filter(src, avfilter_get_by_name("buffer"), "in", args.str().c_str(), nullptr,
                                         graph->get());
  if (err < 0) return ff::ToStatus(err, prefix + "buffer");
  err = avfilter_graph_create_filter(sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph->get());
  if (err < 0) return ff::ToStatus(err, prefix + "buffersink");
  return LinkChain(graph->get(), *src, *sink, chain, out_pixel, prefix);
}

class VideoFilter final : public Operator {
 public:
  explicit VideoFilter(JsonValue options) : options_(std::move(options)) {}

  Status Open(const OpenRequest& r) override {
    ctx_.Capture(r);
    filter_ = options_.GetString("filter").value_or("");
    if (filter_.empty()) return Status::InvalidArgument(ctx_.Prefix() + "filter is required");
    if (const ConnectionContract* c = r.OutputContract("out"); c != nullptr && c->video) out_pixel_ = c->video->pixel_format;
    if (out_pixel_.empty()) out_pixel_ = "yuv420p";
    if (ff::PixelFormatFromName(out_pixel_) == AV_PIX_FMT_NONE) {
      return Status::InvalidArgument(ctx_.Prefix() + "unsupported output pixel format '" + out_pixel_ + "'");
    }
    // Validate the chain up front so a typo is a warm-up failure (mutation
    // rejected, 12 §7.2 A5) rather than a node failure on the first frame.
    ff::FilterGraphPtr probe;
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
    return BuildChain(&probe, &src, &sink, 64, 64, AV_PIX_FMT_YUV420P, AVRational{1, 1}, filter_, out_pixel_,
                      ctx_.Prefix());
  }

  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) {
      if (!graph_) return ProcessResult::kContinue;
      if (const int err = av_buffersrc_add_frame_flags(src_, nullptr, 0); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "flush");
      return Drain(req);
    }
    std::string wanted = filter_;
    if (req.parameters != nullptr) {
      if (const auto f = req.parameters->GetString("filter"); f && !f->empty()) wanted = *f;
    }
    for (const PacketRef& in : req.inputs) {
      if (in->is_event()) continue;
      const AVFrame* frame = FrameOf(*in);
      if (frame == nullptr) return Status::InvalidArgument(ctx_.Prefix() + "input is not a video frame");
      const bool geometry_changed = frame->width != in_w_ || frame->height != in_h_ || frame->format != in_fmt_;
      if (!graph_ || geometry_changed || wanted != active_) {
        if (graph_) {
          if (const int err = av_buffersrc_add_frame_flags(src_, nullptr, 0); err < 0) {
            return ff::ToStatus(err, ctx_.Prefix() + "flush old graph");
          }
          if (Result<ProcessResult> r = Drain(req); !r.ok()) return r;
        }
        active_ = wanted;
        if (Status s = Build(*frame); !s.ok()) return s;
      }
      ff::FramePtr copy = ff::NewFrame();
      if (const int err = av_frame_ref(copy.get(), frame); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "frame_ref");
      if (copy->pts == AV_NOPTS_VALUE) copy->pts = in->header.pts_ns;
      keyframe_pending_ = keyframe_pending_ || (in->header.flags & GE_PACKET_FLAG_KEYFRAME) != 0;
      if (const int err = av_buffersrc_add_frame_flags(src_, copy.get(), AV_BUFFERSRC_FLAG_KEEP_REF); err < 0) {
        return ff::ToStatus(err, ctx_.Prefix() + "buffersrc");
      }
      if (Result<ProcessResult> r = Drain(req); !r.ok()) return r;
    }
    return ProcessResult::kContinue;
  }

  Status Close(const CloseRequest&) override {
    graph_.reset();
    return Status::Ok();
  }

 private:
  Status Build(const AVFrame& sample) {
    in_w_ = sample.width;
    in_h_ = sample.height;
    in_fmt_ = sample.format;
    const AVRational sar = sample.sample_aspect_ratio.num == 0 ? AVRational{1, 1} : sample.sample_aspect_ratio;
    return BuildChain(&graph_, &src_, &sink_, in_w_, in_h_, in_fmt_, sar, active_, out_pixel_, ctx_.Prefix());
  }

  Result<ProcessResult> Drain(const ProcessRequest& req) {
    for (;;) {
      ff::FramePtr out = ff::NewFrame();
      const int err = av_buffersink_get_frame(sink_, out.get());
      if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return ProcessResult::kContinue;
      if (err < 0) return ff::ToStatus(err, ctx_.Prefix() + "buffersink get");
      const std::int64_t pts_ns = out->pts;
      out->time_base = ff::kNanosecond;
      std::uint32_t flags = 0;
      if (keyframe_pending_ || ff::IsKeyFrame(*out)) flags |= GE_PACKET_FLAG_KEYFRAME;
      keyframe_pending_ = false;
      MediaFormat fmt = FormatOfFrame(*out, MediaKind::kVideo, ff::kNanosecond);
      Packet p = MakePacket(WrapFrame(std::move(out)), Tag(kTagVideoFrame), ++seq_, pts_ns, pts_ns, flags);
      SetFormat(&p, fmt);
      if (Status s = req.sink->Emit("out", std::move(p)); !s.ok()) return s;
    }
  }

  JsonValue options_;
  NodeContext ctx_;
  std::string filter_;   // from options
  std::string active_;   // chain the live graph was built with
  std::string out_pixel_;
  ff::FilterGraphPtr graph_;
  AVFilterContext* src_ = nullptr;
  AVFilterContext* sink_ = nullptr;
  int in_w_ = 0, in_h_ = 0, in_fmt_ = -1;
  bool keyframe_pending_ = false;
  PacketSeq seq_ = 0;
};

}  // namespace

Status ValidateFilterChain(std::string_view chain) {
  ff::FilterGraphPtr probe;
  AVFilterContext* src = nullptr;
  AVFilterContext* sink = nullptr;
  return BuildChain(&probe, &src, &sink, 64, 64, AV_PIX_FMT_YUV420P, AVRational{1, 1}, std::string(chain), "yuv420p",
                    "filter chain: ");
}

std::unique_ptr<Operator> MakeVideoFilter(const OperatorCreateArgs& args) {
  return std::make_unique<VideoFilter>(args.options);
}

}  // namespace ge::media
