#include <algorithm>

#include "media/operators_internal.h"

namespace ge::media {

namespace {

struct EncodeParams {
  std::int64_t bitrate_kbps = 2000;
  int gop = 60;
  friend bool operator==(const EncodeParams&, const EncodeParams&) = default;

  static EncodeParams From(const JsonValue* v, EncodeParams base) {
    if (v == nullptr) return base;
    if (const auto b = v->GetInteger("bitrate_kbps"); b && *b > 0) base.bitrate_kbps = *b;
    if (const auto g = v->GetInteger("gop"); g && *g > 0) base.gop = static_cast<int>(*g);
    return base;
  }
};

// libavcodec video encoder.
//
//  * Opens lazily on the first frame (geometry/pixel format come from the
//    negotiated contract and the frame itself).
//  * Hot updates (12 §5): |bitrate_kbps| and |gop| take effect at the next
//    packet boundary by draining and reopening the encoder; the first
//    frame after a reopen is an IDR. |force_idr| (one-shot) marks the next
//    frame as I so a fresh consumer (new rendition, codec switch) can start
//    there without waiting for the GOP (03 TR-A-3 / TR-U-3).
//  * Publishes "media_format_changed" (12 §4.5) with the seq of the
//    keyframe that starts a new configuration; the keyframe packet carries
//    codecpar so lazily opened muxers can write their header from it.
class VideoEncode final : public Operator {
 public:
  explicit VideoEncode(JsonValue options) : options_(std::move(options)) {}

  Status Open(const OpenRequest& r) override {
    ctx_.Capture(r);
    if (Status s = RejectNonSoftwareBackend(options_, ctx_.external_id); !s.ok()) return s;
    codec_name_ = options_.GetString("codec").value_or("h264");
    preset_ = options_.GetString("preset").value_or("veryfast");
    fps_ = static_cast<int>(options_.GetInteger("fps").value_or(0));
    params_ = EncodeParams::From(&options_, EncodeParams{});
    if (const ConnectionContract* c = r.InputContract("in"); c != nullptr && c->video) in_pixel_ = c->video->pixel_format;
    encoder_ = FindVideoEncoder(codec_name_, CodecBackend::kSoftware);
    if (encoder_ == nullptr) return Status::NotFound(ctx_.Prefix() + "no encoder for codec '" + codec_name_ + "'");
    return Status::Ok();
  }

  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) {
      if (!codec_) return ProcessResult::kContinue;
      return DrainEncoder(req, true);
    }
    const EncodeParams wanted = EncodeParams::From(req.parameters, params_);
    const bool force_idr = req.parameters != nullptr && req.parameters->GetBool("force_idr").value_or(false);
    for (const PacketRef& in : req.inputs) {
      if (in->is_event()) continue;
      const AVFrame* frame = FrameOf(*in);
      if (frame == nullptr) return Status::InvalidArgument(ctx_.Prefix() + "input is not a video frame");
      if (codec_ && (wanted != params_ || frame->width != codec_->width || frame->height != codec_->height)) {
        // Parameter/geometry change: finish the current stream, then reopen
        // so the next frame starts a new configuration with an IDR.
        if (Result<ProcessResult> r = DrainEncoder(req, false); !r.ok()) return r;
        codec_.reset();
      }
      params_ = wanted;
      if (!codec_) {
        if (Status s = OpenEncoder(*frame, in->header); !s.ok()) return s;
      }
      ff::FramePtr copy = ff::NewFrame();
      if (const int err = av_frame_ref(copy.get(), frame); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "frame_ref");
      copy->pts = ff::FromNs(in->header.pts_ns, codec_->time_base);
      // One IDR per parameter version that asks for it: a repeated
      // SetParameters({force_idr:true}) bumps the version and asks again.
      if (force_idr && req.parameter_version != idr_served_version_) {
        copy->pict_type = AV_PICTURE_TYPE_I;
        ff::SetKeyFrame(*copy, true);
        idr_served_version_ = req.parameter_version;
      } else {
        copy->pict_type = AV_PICTURE_TYPE_NONE;
      }
      const int err = avcodec_send_frame(codec_.get(), copy.get());
      if (err < 0) return ff::ToStatus(err, ctx_.Prefix() + "send_frame");
      ++frames_in_;
      if (Result<ProcessResult> r = Receive(req); !r.ok()) return r;
    }
    return ProcessResult::kContinue;
  }

  Status Close(const CloseRequest&) override {
    codec_.reset();
    return Status::Ok();
  }

 private:
  Status OpenEncoder(const AVFrame& frame, const PacketHeader& header) {
    codec_.reset(avcodec_alloc_context3(encoder_));
    if (!codec_) return Status::ResourceExhausted(ctx_.Prefix() + "alloc encoder");
    codec_->width = frame.width;
    codec_->height = frame.height;
    codec_->pix_fmt = static_cast<AVPixelFormat>(frame.format);
    codec_->sample_aspect_ratio = frame.sample_aspect_ratio.num == 0 ? AVRational{1, 1} : frame.sample_aspect_ratio;
    // Frames carry ns timestamps; a 1/90000 base keeps packets container
    // friendly and lossless for common frame rates. MPEG-4 part 2 caps the
    // denominator at 65535, so it gets 1/(fps*1000) instead.
    const int fps = fps_ > 0 ? fps_ : 30;
    codec_->time_base = encoder_->id == AV_CODEC_ID_MPEG4 ? AVRational{1, std::min(fps * 1000, 65535)} : AVRational{1, 90000};
    codec_->framerate = AVRational{fps, 1};
    codec_->bit_rate = params_.bitrate_kbps * 1000;
    codec_->gop_size = params_.gop;
    codec_->max_b_frames = 0;  // no reordering: dts == pts, first packet is the IDR
    codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (encoder_->id == AV_CODEC_ID_H264 && std::string(encoder_->name) == "libx264") {
      (void)av_opt_set(codec_->priv_data, "preset", preset_.c_str(), 0);
      (void)av_opt_set(codec_->priv_data, "tune", "zerolatency", 0);
      (void)av_opt_set(codec_->priv_data, "x264-params", "scenecut=0:open-gop=0", 0);
      codec_->keyint_min = params_.gop;
    }
    (void)header;
    if (const int err = avcodec_open2(codec_.get(), encoder_, nullptr); err < 0) {
      codec_.reset();
      return ff::ToStatus(err, ctx_.Prefix() + "avcodec_open2(" + encoder_->name + ")");
    }
    ff::CodecParametersPtr par(avcodec_parameters_alloc());
    if (const int err = avcodec_parameters_from_context(par.get(), codec_.get()); err < 0) {
      return ff::ToStatus(err, ctx_.Prefix() + "parameters_from_context");
    }
    codecpar_ = CodecParToJson(*par);
    format_ = FormatOfCodecPar(*par, ff::kNanosecond);
    format_.codec = avcodec_get_name(encoder_->id);
    format_.frame_rate = Rational{fps, 1};
    config_announced_ = false;
    frames_in_ = 0;
    return Status::Ok();
  }

  Result<ProcessResult> DrainEncoder(const ProcessRequest& req, bool /*final*/) {
    if (const int err = avcodec_send_frame(codec_.get(), nullptr); err < 0 && err != AVERROR_EOF) {
      return ff::ToStatus(err, ctx_.Prefix() + "flush");
    }
    return Receive(req);
  }

  Result<ProcessResult> Receive(const ProcessRequest& req) {
    for (;;) {
      ff::PacketPtr pkt = ff::NewPacket();
      const int err = avcodec_receive_packet(codec_.get(), pkt.get());
      if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return ProcessResult::kContinue;
      if (err < 0) return ff::ToStatus(err, ctx_.Prefix() + "receive_packet");
      const std::int64_t pts_ns = ff::ToNs(pkt->pts, codec_->time_base);
      const std::int64_t dts_ns = ff::ToNs(pkt->dts, codec_->time_base);
      const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
      const PacketSeq seq = ++seq_;
      if (key && !config_announced_ && req.events != nullptr) {
        // 12 §4.5: announce ahead of the keyframe that starts the new
        // configuration (same call, before the emit).
        JsonObject detail;
        detail.emplace("codec", JsonValue(format_.codec));
        detail.emplace("width", JsonValue(format_.width));
        detail.emplace("height", JsonValue(format_.height));
        detail.emplace("pixel_format", JsonValue(format_.pixel_format));
        detail.emplace("bitrate_kbps", JsonValue(params_.bitrate_kbps));
        detail.emplace("gop", JsonValue(params_.gop));
        detail.emplace("first_key_seq", JsonValue(seq));
        detail.emplace("pts_ns", JsonValue(pts_ns));
        req.events->Publish(kEventMediaFormatChanged, Severity::kInfo, JsonValue(std::move(detail)));
        config_announced_ = true;
      }
      pkt->time_base = codec_->time_base;
      Packet out = MakePacket(WrapPacket(std::move(pkt)), Tag(kTagEncodedVideo), seq, pts_ns, dts_ns,
                              key ? GE_PACKET_FLAG_KEYFRAME : 0u);
      SetFormat(&out, format_, key ? &codecpar_ : nullptr);
      if (Status s = req.sink->Emit("out", std::move(out)); !s.ok()) return s;
    }
  }

  JsonValue options_;
  NodeContext ctx_;
  std::string codec_name_, preset_, in_pixel_;
  int fps_ = 0;
  EncodeParams params_;
  const AVCodec* encoder_ = nullptr;
  ff::CodecContextPtr codec_;
  JsonValue codecpar_ = JsonValue(JsonObject{});
  MediaFormat format_;
  bool config_announced_ = false;
  ParameterVersion idr_served_version_ = 0;
  std::int64_t frames_in_ = 0;
  PacketSeq seq_ = 0;
};

}  // namespace

std::unique_ptr<Operator> MakeVideoEncode(const OperatorCreateArgs& args) {
  return std::make_unique<VideoEncode>(args.options);
}

}  // namespace ge::media
