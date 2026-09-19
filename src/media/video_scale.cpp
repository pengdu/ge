#include <sstream>

#include "media/operators_internal.h"

namespace ge::media {

namespace {

struct Watermark {
  int x = 0, y = 0, w = 0, h = 0;
  std::string color = "white";
  bool enabled = false;
  friend bool operator==(const Watermark&, const Watermark&) = default;

  static Watermark From(const JsonValue* v) {
    Watermark m;
    if (v == nullptr || !v->is_object()) return m;
    m.enabled = v->GetBool("enabled").value_or(true);
    m.x = static_cast<int>(v->GetInteger("x").value_or(0));
    m.y = static_cast<int>(v->GetInteger("y").value_or(0));
    m.w = static_cast<int>(v->GetInteger("w").value_or(0));
    m.h = static_cast<int>(v->GetInteger("h").value_or(0));
    m.color = v->GetString("color").value_or("white");
    if (m.w <= 0 || m.h <= 0) m.enabled = false;
    return m;
  }
};

// scale (+ drawbox watermark) through libavfilter. The graph is (re)built
// on the first frame and whenever the input geometry or the hot-updatable
// watermark changes (12 §5: parameter switches happen at packet
// boundaries, so a rebuild is always between two frames).
class VideoScale final : public Operator {
 public:
  explicit VideoScale(JsonValue options) : options_(std::move(options)) {}

  Status Open(const OpenRequest& r) override {
    ctx_.Capture(r);
    width_ = static_cast<int>(options_.GetInteger("width").value_or(0));
    height_ = static_cast<int>(options_.GetInteger("height").value_or(0));
    if (width_ <= 0 || height_ <= 0) return Status::InvalidArgument(ctx_.Prefix() + "width/height are required");
    if (const ConnectionContract* c = r.OutputContract("out"); c != nullptr && c->video) out_pixel_ = c->video->pixel_format;
    if (out_pixel_.empty()) out_pixel_ = "yuv420p";
    if (ff::PixelFormatFromName(out_pixel_) == AV_PIX_FMT_NONE) {
      return Status::InvalidArgument(ctx_.Prefix() + "unsupported output pixel format '" + out_pixel_ + "'");
    }
    return Status::Ok();
  }

  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) {
      if (!graph_) return ProcessResult::kContinue;
      if (const int err = av_buffersrc_add_frame_flags(src_, nullptr, 0); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "flush");
      return Drain(req);
    }
    const Watermark wm = Watermark::From(req.parameters != nullptr ? req.parameters->Find("watermark") : nullptr);
    for (const PacketRef& in : req.inputs) {
      if (in->is_event()) continue;
      const AVFrame* frame = FrameOf(*in);
      if (frame == nullptr) return Status::InvalidArgument(ctx_.Prefix() + "input is not a video frame");
      const bool geometry_changed = frame->width != in_w_ || frame->height != in_h_ || frame->format != in_fmt_;
      if (!graph_ || geometry_changed || wm != watermark_) {
        // Any frames still inside the old graph are pushed out first so
        // nothing is lost across a rebuild.
        if (graph_) {
          (void)av_buffersrc_add_frame_flags(src_, nullptr, 0);
          if (Result<ProcessResult> r = Drain(req); !r.ok()) return r;
        }
        watermark_ = wm;
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
    graph_.reset(avfilter_graph_alloc());
    if (!graph_) return Status::ResourceExhausted(ctx_.Prefix() + "filter graph alloc");
    in_w_ = sample.width;
    in_h_ = sample.height;
    in_fmt_ = sample.format;
    const AVRational sar = sample.sample_aspect_ratio.num == 0 ? AVRational{1, 1} : sample.sample_aspect_ratio;
    std::ostringstream args;
    args << "video_size=" << in_w_ << "x" << in_h_ << ":pix_fmt=" << in_fmt_
         << ":time_base=1/1000000000:pixel_aspect=" << sar.num << "/" << sar.den;
    int err = avfilter_graph_create_filter(&src_, avfilter_get_by_name("buffer"), "in", args.str().c_str(), nullptr, graph_.get());
    if (err < 0) return ff::ToStatus(err, ctx_.Prefix() + "buffer");
    err = avfilter_graph_create_filter(&sink_, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph_.get());
    if (err < 0) return ff::ToStatus(err, ctx_.Prefix() + "buffersink");
    // The trailing format= filter pins the output pixel format; no
    // buffersink pix_fmts option needed (av_opt_set_int_list is gone in
    // FFmpeg 8+).

    std::ostringstream chain;
    chain << "scale=w=" << width_ << ":h=" << height_ << ":flags=bicubic";
    if (watermark_.enabled) {
      chain << ",drawbox=x=" << watermark_.x << ":y=" << watermark_.y << ":w=" << watermark_.w << ":h=" << watermark_.h
            << ":color=" << watermark_.color << ":t=fill";
    }
    chain << ",format=pix_fmts=" << out_pixel_;

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = src_;
    outputs->pad_idx = 0;
    outputs->next = nullptr;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink_;
    inputs->pad_idx = 0;
    inputs->next = nullptr;
    err = avfilter_graph_parse_ptr(graph_.get(), chain.str().c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (err < 0) return ff::ToStatus(err, ctx_.Prefix() + "parse '" + chain.str() + "'");
    err = avfilter_graph_config(graph_.get(), nullptr);
    if (err < 0) return ff::ToStatus(err, ctx_.Prefix() + "graph config");
    return Status::Ok();
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
  int width_ = 0, height_ = 0;
  std::string out_pixel_;
  ff::FilterGraphPtr graph_;
  AVFilterContext* src_ = nullptr;
  AVFilterContext* sink_ = nullptr;
  int in_w_ = 0, in_h_ = 0, in_fmt_ = -1;
  Watermark watermark_;
  bool keyframe_pending_ = false;
  PacketSeq seq_ = 0;
};

// swscale conversion to the negotiated output pixel format; pass-through
// when the input already matches.
class VideoConvert final : public Operator {
 public:
  Status Open(const OpenRequest& r) override {
    ctx_.Capture(r);
    if (const ConnectionContract* c = r.OutputContract("out"); c != nullptr && c->video) out_pixel_ = c->video->pixel_format;
    if (out_pixel_.empty()) out_pixel_ = "yuv420p";
    want_ = ff::PixelFormatFromName(out_pixel_);
    if (want_ == AV_PIX_FMT_NONE) return Status::InvalidArgument(ctx_.Prefix() + "unsupported pixel format '" + out_pixel_ + "'");
    return Status::Ok();
  }

  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ProcessResult::kContinue;
    for (const PacketRef& in : req.inputs) {
      if (in->is_event()) continue;
      const AVFrame* frame = FrameOf(*in);
      if (frame == nullptr) return Status::InvalidArgument(ctx_.Prefix() + "input is not a video frame");
      if (frame->format == want_) {
        Packet out = *in;
        out.header.seq = ++seq_;
        if (Status s = req.sink->Emit("out", std::move(out)); !s.ok()) return s;
        continue;
      }
      ff::FramePtr out = ff::NewFrame();
      out->format = want_;
      out->width = frame->width;
      out->height = frame->height;
      if (const int err = av_frame_get_buffer(out.get(), 0); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "frame buffer");
      sws_.reset(sws_getCachedContext(sws_.release(), frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                      frame->width, frame->height, want_, SWS_BILINEAR, nullptr, nullptr, nullptr));
      if (!sws_) return Status::Internal(ctx_.Prefix() + "sws context");
      sws_scale(sws_.get(), frame->data, frame->linesize, 0, frame->height, out->data, out->linesize);
      (void)av_frame_copy_props(out.get(), frame);
      MediaFormat fmt = FormatOfFrame(*out, MediaKind::kVideo, ff::kNanosecond);
      Packet p = MakePacket(WrapFrame(std::move(out)), Tag(kTagVideoFrame), ++seq_, in->header.pts_ns, in->header.dts_ns,
                            in->header.flags & GE_PACKET_FLAG_KEYFRAME);
      SetFormat(&p, fmt);
      if (Status s = req.sink->Emit("out", std::move(p)); !s.ok()) return s;
    }
    return ProcessResult::kContinue;
  }

  Status Close(const CloseRequest&) override {
    sws_.reset();
    return Status::Ok();
  }

 private:
  NodeContext ctx_;
  std::string out_pixel_;
  AVPixelFormat want_ = AV_PIX_FMT_NONE;
  ff::SwsContextPtr sws_;
  PacketSeq seq_ = 0;
};

}  // namespace

std::unique_ptr<Operator> MakeVideoScale(const OperatorCreateArgs& args) {
  return std::make_unique<VideoScale>(args.options);
}

std::unique_ptr<Operator> MakeVideoConvert(const OperatorCreateArgs&) { return std::make_unique<VideoConvert>(); }

}  // namespace ge::media
