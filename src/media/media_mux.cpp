#include <deque>
#include <filesystem>

#include "media/operators_internal.h"

namespace ge::media {

namespace {

constexpr std::size_t kMaxPendingBeforeHeader = 256;

// libavformat muxer sink. Streams are created from the codecpar carried by
// the first packet of each connected input; the header is written once
// every connected input delivered its first packet (packets arriving
// before that are held, bounded). FLUSH (drain removal / stop) writes the
// trailer; a fast Close still tries to finish the file so FLV
// previousTagSize / MP4 moov are complete whenever possible (03 TR-D-6).
//
// Segmented output (long-video splitting): with |output_pattern|
// ("dir/out_%03d.mp4") instead of |output_path|, every video packet flagged
// GE_PACKET_FLAG_SEGMENT_START (VideoEncode's segment_duration_ms) rotates
// the file: trailer + close, open the next path, re-create the streams from
// the recorded codecpar, write the header right away. Timestamps restart at
// zero in each segment so every file is independently playable; a
// "media_segment" event {index, path, start_pts_ns} announces each rotation.
class MediaMux final : public Operator {
 public:
  explicit MediaMux(JsonValue options) : options_(std::move(options)) {}

  Status Open(const OpenRequest& r) override {
    ctx_.Capture(r);
    output_pattern_ = options_.GetString("output_pattern").value_or("");
    if (!output_pattern_.empty()) {
      if (output_pattern_.find('%') == std::string::npos) {
        return Status::InvalidArgument(ctx_.Prefix() + "output_pattern needs a printf-style index (e.g. out_%03d.mp4)");
      }
      output_path_ = PathFor(0);
    } else {
      output_path_ = options_.GetString("output_path").value_or("");
    }
    if (output_path_.empty()) return Status::InvalidArgument(ctx_.Prefix() + "output_path or output_pattern is required");
    container_ = options_.GetString("container").value_or("");
    if (container_.empty()) {
      const std::string ext = std::filesystem::path(output_path_).extension().string();
      container_ = ext == ".mp4" ? "mp4" : ext == ".flv" ? "flv" : ext == ".mkv" ? "matroska" : "flv";
    }
    movflags_ = options_.GetString("movflags").value_or("");
    align_start_ = options_.GetBool("align_start").value_or(true);
    video_connected_ = r.InputContract("video") != nullptr;
    audio_connected_ = r.InputContract("audio") != nullptr;
    if (!video_connected_ && !audio_connected_) return Status::InvalidArgument(ctx_.Prefix() + "no input connected");

    AVFormatContext* raw = nullptr;
    const int err = avformat_alloc_output_context2(&raw, nullptr, container_.c_str(), output_path_.c_str());
    if (err < 0 || raw == nullptr) return ff::ToStatus(err < 0 ? err : AVERROR_MUXER_NOT_FOUND, ctx_.Prefix() + "output context '" + container_ + "'");
    fmt_.reset(raw);
    return Status::Ok();
  }

  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) {
      // Drain: whatever is still held goes out (even if a stream never
      // arrived), then the trailer.
      if (!header_written_) {
        if (Status s = WriteHeader(true); !s.ok()) return s;
      }
      return Finish();
    }
    for (std::size_t i = 0; i < req.inputs.size(); ++i) {
      const PacketRef& in = req.inputs[i];
      if (in->is_event()) continue;
      const std::string_view port = req.input_ports[i];
      Stream& st = port == "audio" ? audio_ : video_;
      if (st.index < 0) {
        if (Status s = AddStream(st, *in, port); !s.ok()) return s;
        if (port == "video") {
          first_video_pts_ns_ = in->header.pts_ns;
          if (!output_pattern_.empty()) segment_start_pts_ns_ = in->header.pts_ns;
        }
      }
      if (!output_pattern_.empty() && port == "video" && header_written_ &&
          (in->header.flags & GE_PACKET_FLAG_SEGMENT_START) != 0) {
        if (Status s = Rotate(*in, req.events); !s.ok()) return s;
      }
      if (!header_written_) {
        pending_.emplace_back(port == "audio", in);
        if (pending_.size() > kMaxPendingBeforeHeader) {
          return Status::ResourceExhausted(ctx_.Prefix() + "no header after " + std::to_string(kMaxPendingBeforeHeader) + " packets");
        }
        if (AllStreamsKnown()) {
          if (Status s = WriteHeader(false); !s.ok()) return s;
        }
        continue;
      }
      if (Status s = Write(st, *in); !s.ok()) return s;
    }
    return ProcessResult::kContinue;
  }

  Status Close(const CloseRequest&) override {
    Status s = Status::Ok();
    if (fmt_ && header_written_ && !trailer_written_) s = Finish().status();
    fmt_.reset();
    return s;
  }

 private:
  struct Stream {
    int index = -1;
    AVRational time_base{1, 90000};
    std::int64_t last_dts = INT64_MIN;
    std::int64_t packets = 0;
  };

  bool AllStreamsKnown() const {
    return (!video_connected_ || video_.index >= 0) && (!audio_connected_ || audio_.index >= 0);
  }

  Status AddStream(Stream& st, const Packet& first, std::string_view port) {
    const std::string* json = CodecParJson(first);
    if (json == nullptr) return Status::InvalidArgument(ctx_.Prefix() + "first '" + std::string(port) + "' packet carries no codecpar");
    (port == "audio" ? audio_codecpar_json_ : video_codecpar_json_) = *json;
    JsonParseResult parsed = ParseJson(*json);
    if (!parsed.ok()) return Status::InvalidArgument(ctx_.Prefix() + "codecpar: " + parsed.error);
    AVStream* s = avformat_new_stream(fmt_.get(), nullptr);
    if (s == nullptr) return Status::ResourceExhausted(ctx_.Prefix() + "new stream");
    if (Status r = CodecParFromJson(*parsed.value, s->codecpar); !r.ok()) return r;
    s->codecpar->codec_tag = 0;
    const AVPacket* pkt = PacketOf(first);
    s->time_base = pkt != nullptr && pkt->time_base.num > 0 ? pkt->time_base : AVRational{1, 90000};
    if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && s->codecpar->sample_rate > 0) {
      s->time_base = AVRational{1, s->codecpar->sample_rate};
    }
    st.index = s->index;
    st.time_base = s->time_base;
    return Status::Ok();
  }

  Status WriteHeader(bool partial) {
    if (!(fmt_->oformat->flags & AVFMT_NOFILE)) {
      if (const int err = avio_open(&fmt_->pb, output_path_.c_str(), AVIO_FLAG_WRITE); err < 0) {
        return ff::ToStatus(err, ctx_.Prefix() + "open '" + output_path_ + "'");
      }
    }
    AVDictionary* opts = nullptr;
    if (!movflags_.empty()) av_dict_set(&opts, "movflags", movflags_.c_str(), 0);
    const int err = avformat_write_header(fmt_.get(), &opts);
    av_dict_free(&opts);
    if (err < 0) return ff::ToStatus(err, ctx_.Prefix() + "write_header");
    header_written_ = true;
    (void)partial;
    for (auto& [is_audio, pkt] : pending_) {
      Stream& st = is_audio ? audio_ : video_;
      if (st.index < 0) continue;
      if (Status s = Write(st, *pkt); !s.ok()) return s;
    }
    pending_.clear();
    return Status::Ok();
  }

  // 15 §9 task 4 (A/V alignment for a rendition that starts mid-stream):
  // the branch starts at the encoder's first IDR; audio frames that end
  // before that instant are dropped so the first audio sample lands within
  // one AAC frame (~21ms) of the first video frame (03 TR-A-4 <= 40ms).
  bool TrimLeadingAudio(const Packet& in, const AVPacket* src) const {
    if (!align_start_ || first_video_pts_ns_ == INT64_MIN || in.header.pts_ns == INT64_MIN) return false;
    const std::int64_t duration_ns =
        src->duration > 0 && src->time_base.num > 0 ? av_rescale_q(src->duration, src->time_base, ff::kNanosecond) : 0;
    return in.header.pts_ns + duration_ns <= first_video_pts_ns_;
  }

  Status Write(Stream& st, const Packet& in) {
    const AVPacket* src = PacketOf(in);
    if (src == nullptr) return Status::InvalidArgument(ctx_.Prefix() + "input is not an encoded packet");
    if (&st == &audio_ && audio_.packets == 0 && TrimLeadingAudio(in, src)) {
      ++trimmed_audio_;
      return Status::Ok();
    }
    ff::PacketPtr pkt = ff::NewPacket();
    if (const int err = av_packet_ref(pkt.get(), src); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "packet_ref");
    AVStream* s = fmt_->streams[st.index];
    pkt->stream_index = st.index;
    // Segmented files restart at zero so each is independently playable.
    const std::int64_t base = output_pattern_.empty() || segment_start_pts_ns_ == INT64_MIN ? 0 : segment_start_pts_ns_;
    pkt->pts = ff::FromNs(in.header.pts_ns - base, s->time_base);
    pkt->dts = ff::FromNs((in.header.dts_ns == INT64_MIN ? in.header.pts_ns : in.header.dts_ns) - base, s->time_base);
    if (base > 0) {
      // A packet straddling the split (audio frame, B-free video never
      // does) is clamped rather than written with a negative timestamp.
      if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < 0) pkt->pts = 0;
      if (pkt->dts != AV_NOPTS_VALUE && pkt->dts < 0) pkt->dts = 0;
    }
    if (pkt->dts != AV_NOPTS_VALUE && st.last_dts != INT64_MIN && pkt->dts <= st.last_dts) pkt->dts = st.last_dts + 1;
    if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) pkt->pts = pkt->dts;
    st.last_dts = pkt->dts;
    if (src->time_base.num > 0 && src->duration > 0) pkt->duration = av_rescale_q(src->duration, src->time_base, s->time_base);
    pkt->time_base = s->time_base;
    if (const int err = av_interleaved_write_frame(fmt_.get(), pkt.get()); err < 0) {
      return ff::ToStatus(err, ctx_.Prefix() + "write_frame");
    }
    ++st.packets;
    return Status::Ok();
  }

  [[nodiscard]] std::string PathFor(int index) const {
    char buf[1024];
    std::snprintf(buf, sizeof(buf), output_pattern_.c_str(), index);
    return std::string(buf);
  }

  Status Rotate(const Packet& split, EventSink* events) {
    if (Result<ProcessResult> r = Finish(); !r.ok()) return r.status();
    ++segment_index_;
    output_path_ = PathFor(segment_index_);
    AVFormatContext* raw = nullptr;
    const int err = avformat_alloc_output_context2(&raw, nullptr, container_.c_str(), output_path_.c_str());
    if (err < 0 || raw == nullptr) {
      return ff::ToStatus(err < 0 ? err : AVERROR_MUXER_NOT_FOUND, ctx_.Prefix() + "segment output context");
    }
    fmt_.reset(raw);
    header_written_ = false;
    trailer_written_ = false;
    const auto reopen = [&](Stream& st, const std::string& codecpar_json) -> Status {
      if (st.index < 0 || codecpar_json.empty()) return Status::Ok();
      JsonParseResult parsed = ParseJson(codecpar_json);
      if (!parsed.ok()) return Status::Internal(ctx_.Prefix() + "recorded codecpar: " + parsed.error);
      AVStream* s = avformat_new_stream(fmt_.get(), nullptr);
      if (s == nullptr) return Status::ResourceExhausted(ctx_.Prefix() + "new stream");
      if (Status r = CodecParFromJson(*parsed.value, s->codecpar); !r.ok()) return r;
      s->codecpar->codec_tag = 0;
      s->time_base = st.time_base;
      st.index = s->index;
      st.last_dts = INT64_MIN;
      st.packets = 0;
      return Status::Ok();
    };
    // Streams are re-created in the original order so indices are stable.
    if (video_.index >= 0 && audio_.index >= 0 && audio_.index < video_.index) {
      if (Status s = reopen(audio_, audio_codecpar_json_); !s.ok()) return s;
      if (Status s = reopen(video_, video_codecpar_json_); !s.ok()) return s;
    } else {
      if (Status s = reopen(video_, video_codecpar_json_); !s.ok()) return s;
      if (Status s = reopen(audio_, audio_codecpar_json_); !s.ok()) return s;
    }
    segment_start_pts_ns_ = split.header.pts_ns;
    // The new segment starts at the split IDR: leading audio is trimmed
    // against it (align_start), same as a rendition that starts mid-stream.
    first_video_pts_ns_ = split.header.pts_ns;
    if (Status s = WriteHeader(false); !s.ok()) return s;
    if (events != nullptr) {
      events->Publish(kEventMediaSegment, Severity::kInfo,
                      JsonValue(JsonObject{{"index", JsonValue(segment_index_)},
                                           {"path", JsonValue(output_path_)},
                                           {"start_pts_ns", JsonValue(split.header.pts_ns)}}));
    }
    return Status::Ok();
  }

  Result<ProcessResult> Finish() {
    if (!header_written_ || trailer_written_) return ProcessResult::kContinue;
    trailer_written_ = true;
    if (const int err = av_write_trailer(fmt_.get()); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "write_trailer");
    if (fmt_->pb != nullptr && !(fmt_->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt_->pb);
    return ProcessResult::kContinue;
  }

  JsonValue options_;
  NodeContext ctx_;
  std::string output_path_, output_pattern_, container_, movflags_;
  std::string video_codecpar_json_, audio_codecpar_json_;
  int segment_index_ = 0;
  std::int64_t segment_start_pts_ns_ = INT64_MIN;
  bool video_connected_ = false, audio_connected_ = false, align_start_ = true;
  std::int64_t first_video_pts_ns_ = INT64_MIN;
  std::int64_t trimmed_audio_ = 0;
  ff::OutputFormatPtr fmt_;
  Stream video_, audio_;
  bool header_written_ = false, trailer_written_ = false;
  std::deque<std::pair<bool, PacketRef>> pending_;
};

}  // namespace

std::unique_ptr<Operator> MakeMediaMux(const OperatorCreateArgs& args) {
  return std::make_unique<MediaMux>(args.options);
}

}  // namespace ge::media
