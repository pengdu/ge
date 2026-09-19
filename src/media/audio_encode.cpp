#include "media/operators_internal.h"

namespace ge::media {

namespace {

// AAC encoder. Input frames of any sample format/rate are resampled into
// the encoder's native format and buffered in an AVAudioFifo so every
// encoder call sees exactly frame_size samples. Timestamps are derived
// from the sample counter (continuous audio timeline); the first input
// pts anchors the timeline.
class AudioEncode final : public Operator {
 public:
  explicit AudioEncode(JsonValue options) : options_(std::move(options)) {}

  Status Open(const OpenRequest& r) override {
    ctx_.Capture(r);
    bitrate_kbps_ = options_.GetInteger("bitrate_kbps").value_or(128);
    sample_rate_ = static_cast<int>(options_.GetInteger("sample_rate").value_or(0));
    channels_ = static_cast<int>(options_.GetInteger("channels").value_or(0));
    encoder_ = avcodec_find_encoder_by_name("aac");
    if (encoder_ == nullptr) encoder_ = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (encoder_ == nullptr) return Status::NotFound(ctx_.Prefix() + "no AAC encoder");
    return Status::Ok();
  }

  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) {
      if (!codec_) return ProcessResult::kContinue;
      if (Result<ProcessResult> r = EncodeFifo(req, true); !r.ok()) return r;
      if (const int err = avcodec_send_frame(codec_.get(), nullptr); err < 0 && err != AVERROR_EOF) {
        return ff::ToStatus(err, ctx_.Prefix() + "flush");
      }
      return Receive(req);
    }
    for (const PacketRef& in : req.inputs) {
      if (in->is_event()) continue;
      const AVFrame* frame = FrameOf(*in);
      if (frame == nullptr) return Status::InvalidArgument(ctx_.Prefix() + "input is not an audio frame");
      if (!codec_) {
        if (Status s = OpenEncoder(*frame, in->header.pts_ns); !s.ok()) return s;
      }
      // Resample into the encoder's format.
      ff::FramePtr conv = ff::NewFrame();
      conv->format = codec_->sample_fmt;
      conv->sample_rate = codec_->sample_rate;
      if (const int err = av_channel_layout_copy(&conv->ch_layout, &codec_->ch_layout); err < 0) return ff::ToStatus(err, "layout");
      if (const int err = swr_convert_frame(swr_.get(), conv.get(), frame); err < 0) {
        return ff::ToStatus(err, ctx_.Prefix() + "swr convert");
      }
      if (conv->nb_samples > 0) {
        if (const int err = av_audio_fifo_write(fifo_.get(), reinterpret_cast<void**>(conv->data), conv->nb_samples); err < 0) {
          return ff::ToStatus(err, ctx_.Prefix() + "fifo write");
        }
      }
      if (Result<ProcessResult> r = EncodeFifo(req, false); !r.ok()) return r;
    }
    return ProcessResult::kContinue;
  }

  Status Close(const CloseRequest&) override {
    codec_.reset();
    swr_.reset();
    fifo_.reset();
    return Status::Ok();
  }

 private:
  Status OpenEncoder(const AVFrame& first, std::int64_t first_pts_ns) {
    codec_.reset(avcodec_alloc_context3(encoder_));
    if (!codec_) return Status::ResourceExhausted(ctx_.Prefix() + "alloc encoder");
    codec_->sample_fmt = ff::FirstSupportedSampleFormat(encoder_);
    if (codec_->sample_fmt == AV_SAMPLE_FMT_NONE) codec_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    codec_->sample_rate = sample_rate_ > 0 ? sample_rate_ : first.sample_rate;
    if (channels_ > 0) {
      av_channel_layout_default(&codec_->ch_layout, channels_);
    } else if (const int err = av_channel_layout_copy(&codec_->ch_layout, &first.ch_layout); err < 0) {
      return ff::ToStatus(err, "layout");
    }
    codec_->bit_rate = bitrate_kbps_ * 1000;
    codec_->time_base = AVRational{1, codec_->sample_rate};
    codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (const int err = avcodec_open2(codec_.get(), encoder_, nullptr); err < 0) {
      codec_.reset();
      return ff::ToStatus(err, ctx_.Prefix() + "avcodec_open2(aac)");
    }
    SwrContext* raw = nullptr;
    if (const int err = swr_alloc_set_opts2(&raw, &codec_->ch_layout, codec_->sample_fmt, codec_->sample_rate, &first.ch_layout,
                                            static_cast<AVSampleFormat>(first.format), first.sample_rate, 0, nullptr);
        err < 0) {
      return ff::ToStatus(err, ctx_.Prefix() + "swr alloc");
    }
    swr_.reset(raw);
    if (const int err = swr_init(swr_.get()); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "swr init");
    fifo_.reset(av_audio_fifo_alloc(codec_->sample_fmt, codec_->ch_layout.nb_channels, codec_->frame_size * 4));
    if (!fifo_) return Status::ResourceExhausted(ctx_.Prefix() + "fifo alloc");
    ff::CodecParametersPtr par(avcodec_parameters_alloc());
    if (const int err = avcodec_parameters_from_context(par.get(), codec_.get()); err < 0) {
      return ff::ToStatus(err, ctx_.Prefix() + "parameters_from_context");
    }
    codecpar_ = CodecParToJson(*par);
    format_ = FormatOfCodecPar(*par, ff::kNanosecond);
    format_.codec = "aac";
    base_pts_ns_ = first_pts_ns == INT64_MIN ? 0 : first_pts_ns;
    samples_sent_ = 0;
    return Status::Ok();
  }

  Result<ProcessResult> EncodeFifo(const ProcessRequest& req, bool final) {
    const int frame_size = codec_->frame_size > 0 ? codec_->frame_size : 1024;
    while (av_audio_fifo_size(fifo_.get()) >= frame_size || (final && av_audio_fifo_size(fifo_.get()) > 0)) {
      const int take = std::min(frame_size, av_audio_fifo_size(fifo_.get()));
      ff::FramePtr frame = ff::NewFrame();
      frame->nb_samples = take;
      frame->format = codec_->sample_fmt;
      frame->sample_rate = codec_->sample_rate;
      if (const int err = av_channel_layout_copy(&frame->ch_layout, &codec_->ch_layout); err < 0) return ff::ToStatus(err, "layout");
      if (const int err = av_frame_get_buffer(frame.get(), 0); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "frame buffer");
      if (const int err = av_audio_fifo_read(fifo_.get(), reinterpret_cast<void**>(frame->data), take); err < 0) {
        return ff::ToStatus(err, ctx_.Prefix() + "fifo read");
      }
      frame->pts = samples_sent_;
      samples_sent_ += take;
      const int err = avcodec_send_frame(codec_.get(), frame.get());
      if (err < 0) return ff::ToStatus(err, ctx_.Prefix() + "send_frame");
      if (Result<ProcessResult> r = Receive(req); !r.ok()) return r;
    }
    return ProcessResult::kContinue;
  }

  Result<ProcessResult> Receive(const ProcessRequest& req) {
    for (;;) {
      ff::PacketPtr pkt = ff::NewPacket();
      const int err = avcodec_receive_packet(codec_.get(), pkt.get());
      if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return ProcessResult::kContinue;
      if (err < 0) return ff::ToStatus(err, ctx_.Prefix() + "receive_packet");
      const std::int64_t pts_ns = base_pts_ns_ + ff::ToNs(pkt->pts, codec_->time_base);
      const std::int64_t dts_ns = base_pts_ns_ + ff::ToNs(pkt->dts, codec_->time_base);
      pkt->time_base = codec_->time_base;
      const PacketSeq seq = ++seq_;
      Packet out = MakePacket(WrapPacket(std::move(pkt)), Tag(kTagEncodedAudio), seq, pts_ns, dts_ns, GE_PACKET_FLAG_KEYFRAME);
      SetFormat(&out, format_, &codecpar_);
      if (Status s = req.sink->Emit("out", std::move(out)); !s.ok()) return s;
    }
  }

  JsonValue options_;
  NodeContext ctx_;
  std::int64_t bitrate_kbps_ = 128;
  int sample_rate_ = 0, channels_ = 0;
  const AVCodec* encoder_ = nullptr;
  ff::CodecContextPtr codec_;
  ff::SwrContextPtr swr_;
  ff::AudioFifoPtr fifo_;
  JsonValue codecpar_ = JsonValue(JsonObject{});
  MediaFormat format_;
  std::int64_t base_pts_ns_ = 0, samples_sent_ = 0;
  PacketSeq seq_ = 0;
};

}  // namespace

std::unique_ptr<Operator> MakeAudioEncode(const OperatorCreateArgs& args) {
  return std::make_unique<AudioEncode>(args.options);
}

}  // namespace ge::media
