#include "media/operators_internal.h"

namespace ge::media {

// Lazily opened libavcodec decoder: the codec context is created from the
// codecpar metadata carried by the first packet (12 §2.2a), so the graph
// can be built before the input is probed. FLUSH drains the decoder.
class CodecDecoder final : public Operator {
 public:
  CodecDecoder(MediaKind kind, JsonValue options) : kind_(kind), options_(std::move(options)) {}

  Status Open(const OpenRequest& r) override {
    ctx_.Capture(r);
    if (Status s = RejectNonSoftwareBackend(options_, ctx_.external_id); !s.ok()) return s;
    if (const ConnectionContract* c = r.OutputContract("out")) {
      if (kind_ == MediaKind::kVideo && c->video) wanted_pixel_ = c->video->pixel_format;
      if (kind_ == MediaKind::kAudio && c->audio) wanted_sample_ = c->audio->sample_format;
    }
    return Status::Ok();
  }

  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) {
      if (!codec_) return ProcessResult::kContinue;
      if (const int err = avcodec_send_packet(codec_.get(), nullptr); err < 0 && err != AVERROR_EOF) {
        return ff::ToStatus(err, ctx_.Prefix() + "flush");
      }
      return Receive(req);
    }
    for (const PacketRef& in : req.inputs) {
      if (in->is_event()) continue;
      const AVPacket* pkt = PacketOf(*in);
      if (pkt == nullptr) return Status::InvalidArgument(ctx_.Prefix() + "input is not an encoded packet");
      if (!codec_) {
        if (Status s = OpenCodec(*in); !s.ok()) return s;
      }
      // Timestamps travel in ns through the graph; rescale into the codec
      // time base for the decoder and back on output.
      ff::PacketPtr copy = ff::NewPacket();
      if (const int err = av_packet_ref(copy.get(), pkt); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "packet_ref");
      copy->pts = ff::FromNs(in->header.pts_ns, codec_->pkt_timebase);
      copy->dts = ff::FromNs(in->header.dts_ns, codec_->pkt_timebase);
      const int err = avcodec_send_packet(codec_.get(), copy.get());
      if (err < 0 && err != AVERROR(EAGAIN)) return ff::ToStatus(err, ctx_.Prefix() + "send_packet");
      if (Result<ProcessResult> r = Receive(req); !r.ok()) return r;
    }
    return ProcessResult::kContinue;
  }

  Status Close(const CloseRequest&) override {
    codec_.reset();
    return Status::Ok();
  }

 private:
  Status OpenCodec(const Packet& first) {
    const std::string* json = CodecParJson(first);
    if (json == nullptr) return Status::InvalidArgument(ctx_.Prefix() + "first packet carries no codecpar");
    JsonParseResult parsed = ParseJson(*json);
    if (!parsed.ok()) return Status::InvalidArgument(ctx_.Prefix() + "codecpar: " + parsed.error);
    ff::CodecParametersPtr par(avcodec_parameters_alloc());
    if (Status s = CodecParFromJson(*parsed.value, par.get()); !s.ok()) return s;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (dec == nullptr) return Status::NotFound(ctx_.Prefix() + "no decoder for codec id " + std::to_string(par->codec_id));
    codec_.reset(avcodec_alloc_context3(dec));
    if (!codec_) return Status::ResourceExhausted(ctx_.Prefix() + "alloc codec context");
    if (const int err = avcodec_parameters_to_context(codec_.get(), par.get()); err < 0) {
      return ff::ToStatus(err, ctx_.Prefix() + "parameters_to_context");
    }
    codec_->pkt_timebase = ff::kNanosecond;
    const std::optional<MediaFormat> f = MediaFormat::FromPacket(first);
    if (f) input_format_ = *f;
    if (const int err = avcodec_open2(codec_.get(), dec, nullptr); err < 0) {
      return ff::ToStatus(err, ctx_.Prefix() + "avcodec_open2");
    }
    return Status::Ok();
  }

  Result<ProcessResult> Receive(const ProcessRequest& req) {
    for (;;) {
      ff::FramePtr frame = ff::NewFrame();
      const int err = avcodec_receive_frame(codec_.get(), frame.get());
      if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return ProcessResult::kContinue;
      if (err < 0) return ff::ToStatus(err, ctx_.Prefix() + "receive_frame");
      if (frame->pts == AV_NOPTS_VALUE) frame->pts = frame->best_effort_timestamp;
      const std::int64_t pts_ns = ff::ToNs(frame->pts, codec_->pkt_timebase);
      frame->pts = pts_ns;
      frame->time_base = ff::kNanosecond;
      if (Status s = Convert(&frame); !s.ok()) return s;
      std::uint32_t flags = 0;
      if (ff::IsKeyFrame(*frame)) flags |= GE_PACKET_FLAG_KEYFRAME;
      MediaFormat fmt = FormatOfFrame(*frame, kind_, ff::kNanosecond);
      fmt.frame_rate = input_format_.frame_rate;
      Packet out = MakePacket(WrapFrame(std::move(frame)), Tag(kind_ == MediaKind::kVideo ? kTagVideoFrame : kTagAudioFrame),
                              ++seq_, pts_ns, pts_ns, flags);
      SetFormat(&out, fmt);
      if (Status s = req.sink->Emit("out", std::move(out)); !s.ok()) return s;
    }
  }

  // Honour the negotiated output format when the decoder's native one
  // differs (e.g. yuvj420p source, nv12 contract).
  Status Convert(ff::FramePtr* frame) {
    if (kind_ == MediaKind::kVideo) {
      if (wanted_pixel_.empty()) return Status::Ok();
      const AVPixelFormat want = ff::PixelFormatFromName(wanted_pixel_);
      if (want == AV_PIX_FMT_NONE || (*frame)->format == want) return Status::Ok();
      ff::FramePtr out = ff::NewFrame();
      out->format = want;
      out->width = (*frame)->width;
      out->height = (*frame)->height;
      if (const int err = av_frame_get_buffer(out.get(), 0); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "frame buffer");
      sws_.reset(sws_getCachedContext(sws_.release(), (*frame)->width, (*frame)->height,
                                      static_cast<AVPixelFormat>((*frame)->format), out->width, out->height, want,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr));
      if (!sws_) return Status::Internal(ctx_.Prefix() + "sws context");
      sws_scale(sws_.get(), (*frame)->data, (*frame)->linesize, 0, (*frame)->height, out->data, out->linesize);
      (void)av_frame_copy_props(out.get(), frame->get());
      *frame = std::move(out);
      return Status::Ok();
    }
    if (wanted_sample_.empty()) return Status::Ok();
    const AVSampleFormat want = ff::SampleFormatFromName(wanted_sample_);
    if (want == AV_SAMPLE_FMT_NONE || (*frame)->format == want) return Status::Ok();
    ff::FramePtr out = ff::NewFrame();
    out->format = want;
    out->sample_rate = (*frame)->sample_rate;
    if (const int err = av_channel_layout_copy(&out->ch_layout, &(*frame)->ch_layout); err < 0) return ff::ToStatus(err, "layout");
    if (!swr_) {
      SwrContext* raw = nullptr;
      if (const int err = swr_alloc_set_opts2(&raw, &out->ch_layout, want, out->sample_rate, &(*frame)->ch_layout,
                                              static_cast<AVSampleFormat>((*frame)->format), (*frame)->sample_rate, 0, nullptr);
          err < 0) {
        return ff::ToStatus(err, ctx_.Prefix() + "swr alloc");
      }
      swr_.reset(raw);
      if (const int err = swr_init(swr_.get()); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "swr init");
    }
    if (const int err = swr_convert_frame(swr_.get(), out.get(), frame->get()); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "swr convert");
    (void)av_frame_copy_props(out.get(), frame->get());
    *frame = std::move(out);
    return Status::Ok();
  }

  MediaKind kind_;
  JsonValue options_;
  NodeContext ctx_;
  ff::CodecContextPtr codec_;
  ff::SwsContextPtr sws_;
  ff::SwrContextPtr swr_;
  MediaFormat input_format_;
  std::string wanted_pixel_, wanted_sample_;
  PacketSeq seq_ = 0;
};

std::unique_ptr<Operator> MakeVideoDecode(const OperatorCreateArgs& args) {
  return std::make_unique<CodecDecoder>(MediaKind::kVideo, args.options);
}

std::unique_ptr<Operator> MakeAudioDecode(const OperatorCreateArgs& args) {
  return std::make_unique<CodecDecoder>(MediaKind::kAudio, args.options);
}

}  // namespace ge::media
