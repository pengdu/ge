#include <ge/media/mix.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <vector>

#include "media/operators_internal.h"

namespace ge::media {

namespace {

// P7 (docs/04 §4.4): N-way audio mixing, membership independent of the video
// side (MX-A-1/MX-A-2): the node owns its own ports, so a member can leave
// the picture and stay in the mix, or the other way round.
//
// Every member's frames are resampled into the mix format (the first frame
// that arrives, or the negotiated contract, decides rate/channels/layout) and
// spilled into a contiguous per-member buffer indexed by output sample
// position: a member that skips samples contributes silence there, so a slow
// member neither drifts nor blocks the mix. Each output frame consumes
// frame_size samples from every member's buffer, samples missing at the tail
// count as silence. Per-member gain (0 = muted) is hot-updatable and applies
// from the next frame (MX-A-3).
class AudioMix final : public Operator {
 public:
  explicit AudioMix(JsonValue options) : options_(std::move(options)) {}

  Status Open(const OpenRequest& r) override {
    ctx_.Capture(r);
    if (const ConnectionContract* c = r.OutputContract("out"); c != nullptr && c->audio.has_value()) {
      if (c->audio->sample_rate && !c->audio->sample_rate->Empty()) {
        out_rate_ = static_cast<int>(c->audio->sample_rate->min);
      }
      if (!c->audio->sample_format.empty()) out_sample_fmt_ = c->audio->sample_format;
      if (c->audio->channel_layout) {
        AVChannelLayout layout{};
        if (av_channel_layout_from_string(&layout, c->audio->channel_layout->c_str()) >= 0 && layout.nb_channels > 0) {
          out_channels_ = layout.nb_channels;
        }
        av_channel_layout_uninit(&layout);
      }
    }
    if (out_sample_fmt_.empty()) out_sample_fmt_ = "s16";
    fmt_ = ff::SampleFormatFromName(out_sample_fmt_);
    if (fmt_ != AV_SAMPLE_FMT_S16) {
      return Status::InvalidArgument(ctx_.Prefix() + "mix only produces s16, negotiated '" + out_sample_fmt_ + "'");
    }
    return ApplyParameters(options_);
  }

  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) {
      // All inputs ended: drain whatever is buffered (the < frame_size tail
      // is padded with silence by the mix loop).
      draining_ = true;
      return Mix(req);
    }
    if (req.parameters != nullptr) {
      if (Status s = ApplyParameters(*req.parameters); !s.ok()) return s;
    }
    for (std::size_t i = 0; i < req.inputs.size(); ++i) {
      const PacketRef& in = req.inputs[i];
      if (in->is_event() || in->is_eos()) continue;
      if (i >= req.input_ports.size()) continue;
      const AVFrame* frame = FrameOf(*in);
      if (frame == nullptr) return Status::InvalidArgument(ctx_.Prefix() + "input is not an audio frame");
      if (out_rate_ <= 0) out_rate_ = frame->sample_rate > 0 ? frame->sample_rate : 48000;
      if (out_channels_ <= 0) out_channels_ = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 2;
      Channel& ch = channels_[std::string(req.input_ports[i])];
      if (ch.base_ns == INT64_MIN) ch.base_ns = in->header.pts_ns;
      if (Status s = Append(ch, *frame, in->header.pts_ns); !s.ok()) return s;
    }
    return Mix(req);
  }

  Status Close(const CloseRequest&) override {
    channels_.clear();
    return Status::Ok();
  }

 private:
  struct Channel {
    std::deque<std::int16_t> samples;  // interleaved, mix format
    std::int64_t from = 0;             // output sample index of samples.front()
    std::int64_t filled_to = 0;        // output sample index just past the queue
    std::int64_t base_ns = INT64_MIN;  // pts of the member's first frame
    std::int64_t gain_millis = 1000;
    std::uint64_t frames = 0;
    ff::SwrContextPtr swr;
    int src_format = -1, src_rate = 0, src_channels = 0;
  };

  using Samples = std::deque<std::int16_t>;

  Status ApplyParameters(const JsonValue& params) {
    const JsonValue* gains = params.Find("gains");
    if (gains != nullptr) {
      if (!gains->is_array()) return Status::InvalidArgument(ctx_.Prefix() + "'gains' must be an array");
      for (const JsonValue& g : gains->as_array()) {
        const std::string member = g.GetString("member").value_or("");
        if (member.empty()) return Status::InvalidArgument(ctx_.Prefix() + "gain entry without a member id");
        const std::int64_t gain = g.GetInteger("gain_millis").value_or(1000);
        if (gain < 0 || gain > 10000) {
          return Status::InvalidArgument(ctx_.Prefix() + "gain_millis must be within 0..10000");
        }
        channels_[member].gain_millis = gain;
      }
    }
    if (const auto v = params.GetInteger("frame_size")) {
      if (*v > 0 && *v <= 8192) frame_size_ = static_cast<int>(*v);
    }
    return Status::Ok();
  }

  std::int64_t PositionOf(std::int64_t pts_ns, const Channel& ch) const {
    if (ch.base_ns == INT64_MIN) return 0;
    return (pts_ns - ch.base_ns) * out_rate_ / 1000000000;
  }

  Status Append(Channel& ch, const AVFrame& frame, std::int64_t pts_ns) {
    ff::FramePtr conv = ff::NewFrame();
    conv->format = fmt_;
    conv->sample_rate = out_rate_;
    av_channel_layout_default(&conv->ch_layout, out_channels_);
    conv->nb_samples = frame.nb_samples * out_rate_ / std::max(1, frame.sample_rate) + 64;
    if (const int err = av_frame_get_buffer(conv.get(), 0); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "convert buffer");
    if (ch.swr == nullptr || ch.src_format != frame.format || ch.src_rate != frame.sample_rate ||
        ch.src_channels != frame.ch_layout.nb_channels) {
      SwrContext* raw = nullptr;
      if (const int err = swr_alloc_set_opts2(&raw, &conv->ch_layout, fmt_, out_rate_, &frame.ch_layout,
                                              static_cast<AVSampleFormat>(frame.format), frame.sample_rate, 0, nullptr);
          err < 0) {
        return ff::ToStatus(err, ctx_.Prefix() + "swr alloc");
      }
      ch.swr.reset(raw);
      if (const int err = swr_init(ch.swr.get()); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "swr init");
      ch.src_format = frame.format;
      ch.src_rate = frame.sample_rate;
      ch.src_channels = frame.ch_layout.nb_channels;
    }
    const int got = swr_convert(ch.swr.get(), conv->data, conv->nb_samples,
                                const_cast<const std::uint8_t**>(frame.data), frame.nb_samples);
    if (got < 0) return ff::ToStatus(got, ctx_.Prefix() + "swr convert");
    if (got == 0) return Status::Ok();

    const std::int64_t start = PositionOf(pts_ns, ch);
    if (ch.samples.empty()) {
      ch.from = std::max(ch.filled_to, start);
      ch.filled_to = ch.from;
    } else if (start > ch.filled_to) {
      // A gap: pad with silence so the queue stays contiguous.
      ch.samples.insert(ch.samples.end(), static_cast<std::size_t>((start - ch.filled_to) * out_channels_), 0);
      ch.filled_to = start;
    }
    ReadInterleaved(*conv, static_cast<std::size_t>(got), &ch.samples);
    ch.filled_to = start + got;
    ++ch.frames;
    return Status::Ok();
  }

  void ReadInterleaved(const AVFrame& conv, std::size_t frames, Samples* out) const {
    const std::size_t channels = static_cast<std::size_t>(out_channels_);
    if (srcIsPlanar(static_cast<AVSampleFormat>(conv.format))) {
      for (std::size_t k = 0; k < frames; ++k) {
        for (std::size_t c = 0; c < channels; ++c) {
          const auto* plane = reinterpret_cast<const std::int16_t*>(conv.data[c]);
          out->push_back(plane == nullptr ? 0 : plane[k]);
        }
      }
      return;
    }
    const auto* inter = reinterpret_cast<const std::int16_t*>(conv.data[0]);
    for (std::size_t k = 0; k < frames * channels; ++k) out->push_back(inter == nullptr ? 0 : inter[k]);
  }

  static bool srcIsPlanar(AVSampleFormat f) { return av_sample_fmt_is_planar(f) != 0; }

  Result<ProcessResult> Mix(const ProcessRequest& req) {
    if (out_rate_ <= 0 || out_channels_ <= 0) return ProcessResult::kContinue;
    int produced = 0;
    const int budget = draining_ ? 1 << 20 : kMaxFramesPerCall;
    while (produced < budget) {
      bool any = false;
      for (auto& [port, ch] : channels_) {
        if (!ch.samples.empty()) any = true;
      }
      if (!any) return ProcessResult::kContinue;

      const std::size_t channels = static_cast<std::size_t>(out_channels_);
      std::vector<double> acc(static_cast<std::size_t>(frame_size_) * channels, 0.0);
      for (auto& [port, ch] : channels_) {
        if (ch.gain_millis == 0) {
          Consume(ch, frame_size_);
          continue;
        }
        const double gain = static_cast<double>(ch.gain_millis) / 1000.0;
        const std::size_t available = ch.samples.size() / channels;
        const std::size_t take = std::min<std::size_t>(available, static_cast<std::size_t>(frame_size_));
        for (std::size_t k = 0; k < take * channels; ++k) {
          acc[k] += static_cast<double>(ch.samples[k]) / 32768.0 * gain;
        }
        Consume(ch, frame_size_);
      }

      ff::FramePtr out = ff::NewFrame();
      out->format = fmt_;
      out->sample_rate = out_rate_;
      out->nb_samples = frame_size_;
      av_channel_layout_default(&out->ch_layout, out_channels_);
      if (const int err = av_frame_get_buffer(out.get(), 0); err < 0) return ff::ToStatus(err, ctx_.Prefix() + "mix buffer");
      Store(*out, acc);
      const std::int64_t pts = first_out_pts_ns_ + static_cast<std::int64_t>(samples_out_) * 1000000000 / out_rate_;
      out->pts = pts;
      out->time_base = ff::kNanosecond;
      samples_out_ += static_cast<std::uint64_t>(frame_size_);
      MediaFormat fmt = FormatOfFrame(*out, MediaKind::kAudio, ff::kNanosecond);
      Packet p = MakePacket(WrapFrame(std::move(out)), Tag(kTagAudioFrame), ++seq_, pts, pts, 0);
      SetFormat(&p, fmt);
      if (Status s = req.sink->Emit("out", std::move(p)); !s.ok()) return s;
      ++produced;
    }
    return ProcessResult::kContinue;
  }

  // Drops up to |count| output samples: what the member delivered plus a
  // silent tail for whatever is still missing (the member catches up later
  // into the padding, keeping the mix timeline intact).
  void Consume(Channel& ch, int count) const {
    const std::size_t channels = static_cast<std::size_t>(out_channels_);
    const std::size_t want = static_cast<std::size_t>(count) * channels;
    const std::size_t have = std::min(want, ch.samples.size());
    ch.samples.erase(ch.samples.begin(), ch.samples.begin() + static_cast<std::ptrdiff_t>(have));
    ch.from += count;
    if (ch.samples.empty()) ch.filled_to = std::max(ch.filled_to, ch.from);
  }

  void Store(const AVFrame& frame, const std::vector<double>& acc) const {
    const std::size_t channels = static_cast<std::size_t>(out_channels_);
    for (int k = 0; k < frame.nb_samples; ++k) {
      for (std::size_t c = 0; c < channels; ++c) {
        const double v = acc[static_cast<std::size_t>(k) * channels + c];
        const auto sample = static_cast<std::int16_t>(std::lround(std::max(-1.0, std::min(1.0, v)) * 32767.0));
        if (srcIsPlanar(static_cast<AVSampleFormat>(frame.format))) {
          auto* plane = reinterpret_cast<std::int16_t*>(frame.data[c]);
          if (plane != nullptr) plane[k] = sample;
        } else {
          auto* inter = reinterpret_cast<std::int16_t*>(frame.data[0]);
          if (inter != nullptr) inter[static_cast<std::size_t>(k) * channels + c] = sample;
        }
      }
    }
  }

  static constexpr int kMaxFramesPerCall = 4;

  JsonValue options_;
  NodeContext ctx_;
  std::string out_sample_fmt_ = "s16";
  AVSampleFormat fmt_ = AV_SAMPLE_FMT_S16;
  int out_rate_ = 48000;
  int out_channels_ = 0;
  int frame_size_ = 1024;
  bool draining_ = false;
  std::int64_t first_out_pts_ns_ = 0;
  std::uint64_t samples_out_ = 0;
  std::map<std::string, Channel> channels_;
  PacketSeq seq_ = 0;
};

}  // namespace

std::unique_ptr<Operator> MakeAudioMix(const OperatorCreateArgs& args) { return std::make_unique<AudioMix>(args.options); }

}  // namespace ge::media
