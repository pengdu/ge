#include <chrono>
#include <thread>

#include "media/operators_internal.h"

namespace ge::media {

namespace {

// Source (12 §4.4 step 1): one av_read_frame per Process call; returns
// kExhausted at EOF (or when neither elementary stream is connected). The
// packet keeps the container's timestamps (converted to ns) and carries a
// "format" + "codecpar" metadata so the decoder can open lazily.
class MediaDemux final : public Operator {
 public:
  explicit MediaDemux(JsonValue options) : options_(std::move(options)) {}

  Status Open(const OpenRequest& r) override {
    ctx_.Capture(r);
    const std::optional<std::string> path = r.options != nullptr ? r.options->GetString("input_path") : std::nullopt;
    if (!path || path->empty()) return Status::InvalidArgument(ctx_.Prefix() + "input_path is required");
    realtime_ = r.options->GetBool("realtime").value_or(false);
    loop_ = r.options->GetBool("loop").value_or(false);
    start_ms_ = r.options->GetInteger("start_ms").value_or(0);
    end_ms_ = r.options->GetInteger("end_ms").value_or(0);
    if (start_ms_ < 0 || end_ms_ < 0 || (end_ms_ > 0 && end_ms_ <= start_ms_)) {
      return Status::InvalidArgument(ctx_.Prefix() + "invalid start_ms/end_ms range");
    }
    if ((start_ms_ > 0 || end_ms_ > 0) && loop_) {
      return Status::InvalidArgument(ctx_.Prefix() + "start_ms/end_ms cannot be combined with loop");
    }

    AVFormatContext* raw = nullptr;
    if (const int err = avformat_open_input(&raw, path->c_str(), nullptr, nullptr); err < 0) {
      return ff::ToStatus(err, ctx_.Prefix() + "open '" + *path + "'");
    }
    fmt_.reset(raw);
    if (const int err = avformat_find_stream_info(fmt_.get(), nullptr); err < 0) {
      return ff::ToStatus(err, ctx_.Prefix() + "find_stream_info");
    }
    video_index_ = av_find_best_stream(fmt_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_index_ = av_find_best_stream(fmt_.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (video_index_ < 0 && audio_index_ < 0) {
      return Status::InvalidArgument(ctx_.Prefix() + "no audio or video stream in '" + *path + "'");
    }
    video_connected_ = r.OutputContract("video") != nullptr;
    audio_connected_ = r.OutputContract("audio") != nullptr;
    if (video_index_ >= 0) {
      const AVStream* st = fmt_->streams[video_index_];
      video_format_ = FormatOfCodecPar(*st->codecpar, st->time_base);
      if (video_format_.frame_rate.num == 0) video_format_.frame_rate = FromAv(av_guess_frame_rate(fmt_.get(), fmt_->streams[video_index_], nullptr));
      video_codecpar_ = CodecParToJson(*st->codecpar);
    }
    if (audio_index_ >= 0) {
      const AVStream* st = fmt_->streams[audio_index_];
      audio_format_ = FormatOfCodecPar(*st->codecpar, st->time_base);
      audio_codecpar_ = CodecParToJson(*st->codecpar);
    }
    if (start_ms_ > 0) {
      // Land on the keyframe at or before start so the slice decodes from
      // its first packet; the encoder downstream re-times from the packet
      // timestamps, which stay on the source clock.
      if (const int err = av_seek_frame(fmt_.get(), -1, start_ms_ * 1000, AVSEEK_FLAG_BACKWARD); err < 0) {
        return ff::ToStatus(err, ctx_.Prefix() + "seek to " + std::to_string(start_ms_) + "ms");
      }
    }
    start_wall_ = std::chrono::steady_clock::now();
    return Status::Ok();
  }

  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ProcessResult::kContinue;
    if (!fmt_) return Status::Internal(ctx_.Prefix() + "not open");
    for (;;) {
      ff::PacketPtr pkt = ff::NewPacket();
      const int err = av_read_frame(fmt_.get(), pkt.get());
      if (err == AVERROR_EOF || err == AVERROR(EAGAIN)) {
        if (err == AVERROR_EOF && loop_ && SeekToStart()) continue;
        return ProcessResult::kExhausted;
      }
      if (err < 0) return ff::ToStatus(err, ctx_.Prefix() + "read_frame");
      const bool is_video = pkt->stream_index == video_index_;
      const bool is_audio = pkt->stream_index == audio_index_;
      if ((is_video && !video_connected_) || (is_audio && !audio_connected_) || (!is_video && !is_audio)) continue;
      const AVStream* st = fmt_->streams[pkt->stream_index];
      const std::int64_t pts_ns = ff::ToNs(pkt->pts, st->time_base) + loop_offset_ns_;
      const std::int64_t dts_ns = ff::ToNs(pkt->dts, st->time_base) + loop_offset_ns_;
      if (loop_ && pkt->pts != AV_NOPTS_VALUE) {
        last_pts_ns_ = std::max(last_pts_ns_, pts_ns + ff::ToNs(pkt->duration, st->time_base));
      }
      // End of the requested slice: EOS once the video passed |end_ms_|
      // (audio follows the video cut; a pure-audio graph cuts on audio).
      if (end_ms_ > 0 && pts_ns != INT64_MIN && pts_ns >= end_ms_ * 1000000 &&
          (is_video || video_index_ < 0 || !video_connected_)) {
        return ProcessResult::kExhausted;
      }
      if (realtime_) Pace(is_video ? dts_ns : pts_ns);
      std::uint32_t flags = 0;
      if (pkt->flags & AV_PKT_FLAG_KEY) flags |= GE_PACKET_FLAG_KEYFRAME;
      const PacketSeq seq = is_video ? ++video_seq_ : ++audio_seq_;
      pkt->time_base = st->time_base;
      Packet out = MakePacket(WrapPacket(std::move(pkt)), Tag(is_video ? kTagEncodedVideo : kTagEncodedAudio), seq,
                              pts_ns, dts_ns, flags);
      // The first packet of each stream (and every keyframe) carries the
      // stream configuration so lazily opened consumers can start there.
      const bool first = is_video ? seq == 1 : seq == 1;
      if (first || (flags & GE_PACKET_FLAG_KEYFRAME)) {
        SetFormat(&out, is_video ? video_format_ : audio_format_, is_video ? &video_codecpar_ : &audio_codecpar_);
      } else {
        SetFormat(&out, is_video ? video_format_ : audio_format_);
      }
      Status s = req.sink->Emit(is_video ? "video" : "audio", std::move(out));
      if (!s.ok()) return s;
      return ProcessResult::kContinue;
    }
  }

  Status Close(const CloseRequest&) override {
    fmt_.reset();
    return Status::Ok();
  }

 private:
  bool SeekToStart() {
    if (av_seek_frame(fmt_.get(), -1, 0, AVSEEK_FLAG_BACKWARD) < 0) return false;
    loop_offset_ns_ = last_pts_ns_;
    return true;
  }

  void Pace(std::int64_t media_ns) {
    if (media_ns == INT64_MIN) return;
    const auto target = start_wall_ + std::chrono::nanoseconds(media_ns);
    const auto now = std::chrono::steady_clock::now();
    if (target > now) std::this_thread::sleep_for(std::min(target - now, std::chrono::steady_clock::duration(std::chrono::milliseconds(50))));
  }

  JsonValue options_;
  NodeContext ctx_;
  ff::InputFormatPtr fmt_;
  int video_index_ = -1, audio_index_ = -1;
  bool video_connected_ = false, audio_connected_ = false;
  bool realtime_ = false, loop_ = false;
  std::int64_t start_ms_ = 0, end_ms_ = 0;
  MediaFormat video_format_, audio_format_;
  JsonValue video_codecpar_ = JsonValue(JsonObject{}), audio_codecpar_ = JsonValue(JsonObject{});
  PacketSeq video_seq_ = 0, audio_seq_ = 0;
  std::int64_t loop_offset_ns_ = 0, last_pts_ns_ = 0;
  std::chrono::steady_clock::time_point start_wall_;
};

}  // namespace

std::unique_ptr<Operator> MakeMediaDemux(const OperatorCreateArgs& args) {
  return std::make_unique<MediaDemux>(args.options);
}

}  // namespace ge::media
