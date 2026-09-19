#ifndef GE_SRC_MEDIA_FFMPEG_H_
#define GE_SRC_MEDIA_FFMPEG_H_

// The only header in ge_media that includes FFmpeg. Everything FFmpeg
// specific (RAII wrappers, error translation, time base conversion) lives
// here so operator sources stay -Werror clean and the public headers never
// expose libav* types.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <ge/cpp/types.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// ---------------------------------------------------------------------------
// Version compatibility (docs/11 TD-04). Floor: FFmpeg 6.0 (lavc 60,
// lavu 58) -- the first release with AVChannelLayout, which the audio path
// uses unconditionally. Each GE_FF_* macro names one API difference; the
// helpers below hide it from the operator sources.
// ---------------------------------------------------------------------------

#if LIBAVCODEC_VERSION_MAJOR < 60
#error "ge_media requires FFmpeg >= 6.0 (libavcodec >= 60)"
#endif

// FFmpeg 6.1 (lavu 58.29) adds AV_FRAME_FLAG_KEY and deprecates
// AVFrame::key_frame; FFmpeg 7.0 removes the field. Detect the macro rather
// than the version so 6.1 takes the new path without a deprecation error.
#if defined(AV_FRAME_FLAG_KEY)
#define GE_FF_HAS_FRAME_FLAG_KEY 1
#else
#define GE_FF_HAS_FRAME_FLAG_KEY 0
#endif

// FFmpeg 7.1 (lavc 61.13): AVCodec::pix_fmts/sample_fmts arrays deprecated
// in favour of avcodec_get_supported_config(); FFmpeg 8 removes the arrays.
#define GE_FF_HAS_SUPPORTED_CONFIG (LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100))

namespace ge::media::ff {

// AVFrame keyframe flag, both spellings.
[[nodiscard]] inline bool IsKeyFrame(const AVFrame& f) noexcept {
#if GE_FF_HAS_FRAME_FLAG_KEY
  return (f.flags & AV_FRAME_FLAG_KEY) != 0;
#else
  return f.key_frame != 0;
#endif
}
inline void SetKeyFrame(AVFrame& f, bool key) noexcept {
#if GE_FF_HAS_FRAME_FLAG_KEY
  if (key) {
    f.flags |= AV_FRAME_FLAG_KEY;
  } else {
    f.flags &= ~AV_FRAME_FLAG_KEY;
  }
#else
  f.key_frame = key ? 1 : 0;
#endif
}

// First sample format the encoder advertises, or AV_SAMPLE_FMT_NONE.
[[nodiscard]] inline AVSampleFormat FirstSupportedSampleFormat(const AVCodec* codec) noexcept {
#if GE_FF_HAS_SUPPORTED_CONFIG
  const void* fmts = nullptr;
  int n = 0;
  if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &fmts, &n) >= 0 &&
      fmts != nullptr && n > 0) {
    return static_cast<const AVSampleFormat*>(fmts)[0];
  }
  return AV_SAMPLE_FMT_NONE;
#else
  return codec->sample_fmts != nullptr ? codec->sample_fmts[0] : AV_SAMPLE_FMT_NONE;
#endif
}

// First pixel format the encoder advertises, or AV_PIX_FMT_NONE.
[[nodiscard]] inline AVPixelFormat FirstSupportedPixelFormat(const AVCodec* codec) noexcept {
#if GE_FF_HAS_SUPPORTED_CONFIG
  const void* fmts = nullptr;
  int n = 0;
  if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &fmts, &n) >= 0 &&
      fmts != nullptr && n > 0) {
    return static_cast<const AVPixelFormat*>(fmts)[0];
  }
  return AV_PIX_FMT_NONE;
#else
  return codec->pix_fmts != nullptr ? codec->pix_fmts[0] : AV_PIX_FMT_NONE;
#endif
}

// Runtime library versions (for Snapshot / diagnostics).
[[nodiscard]] std::string LibraryVersions();

// ---------------------------------------------------------------------------
// RAII
// ---------------------------------------------------------------------------

struct FrameDeleter {
  void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
struct PacketDeleter {
  void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct CodecContextDeleter {
  void operator()(AVCodecContext* c) const noexcept { avcodec_free_context(&c); }
};
struct FormatContextInputDeleter {
  void operator()(AVFormatContext* c) const noexcept { avformat_close_input(&c); }
};
struct FormatContextOutputDeleter {
  void operator()(AVFormatContext* c) const noexcept {
    if (c == nullptr) return;
    if (c->pb != nullptr && !(c->oformat->flags & AVFMT_NOFILE)) avio_closep(&c->pb);
    avformat_free_context(c);
  }
};
struct FilterGraphDeleter {
  void operator()(AVFilterGraph* g) const noexcept { avfilter_graph_free(&g); }
};
struct SwsContextDeleter {
  void operator()(SwsContext* s) const noexcept { sws_freeContext(s); }
};
struct SwrContextDeleter {
  void operator()(SwrContext* s) const noexcept { swr_free(&s); }
};
struct AudioFifoDeleter {
  void operator()(AVAudioFifo* f) const noexcept { av_audio_fifo_free(f); }
};
struct CodecParametersDeleter {
  void operator()(AVCodecParameters* p) const noexcept { avcodec_parameters_free(&p); }
};

using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using InputFormatPtr = std::unique_ptr<AVFormatContext, FormatContextInputDeleter>;
using OutputFormatPtr = std::unique_ptr<AVFormatContext, FormatContextOutputDeleter>;
using FilterGraphPtr = std::unique_ptr<AVFilterGraph, FilterGraphDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;
using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;
using CodecParametersPtr = std::unique_ptr<AVCodecParameters, CodecParametersDeleter>;

inline FramePtr NewFrame() { return FramePtr(av_frame_alloc()); }
inline PacketPtr NewPacket() { return PacketPtr(av_packet_alloc()); }

// ---------------------------------------------------------------------------
// Errors / time
// ---------------------------------------------------------------------------

[[nodiscard]] std::string ErrorString(int err);

// Maps an AVERROR to a Status. EAGAIN -> WOULD_BLOCK, EOF -> OK with
// |eof| set by callers that care, everything else -> INTERNAL/INVALID.
[[nodiscard]] Status ToStatus(int err, std::string_view what);

inline constexpr AVRational kNanosecond{1, 1000000000};

[[nodiscard]] inline std::int64_t ToNs(std::int64_t ts, AVRational tb) noexcept {
  if (ts == AV_NOPTS_VALUE) return INT64_MIN;
  return av_rescale_q(ts, tb, kNanosecond);
}
[[nodiscard]] inline std::int64_t FromNs(std::int64_t ns, AVRational tb) noexcept {
  if (ns == INT64_MIN) return AV_NOPTS_VALUE;
  return av_rescale_q(ns, kNanosecond, tb);
}

// ---------------------------------------------------------------------------
// Lookup helpers
// ---------------------------------------------------------------------------

[[nodiscard]] AVPixelFormat PixelFormatFromName(std::string_view name) noexcept;
[[nodiscard]] std::string PixelFormatName(AVPixelFormat fmt);
[[nodiscard]] AVSampleFormat SampleFormatFromName(std::string_view name) noexcept;
[[nodiscard]] std::string SampleFormatName(AVSampleFormat fmt);

}  // namespace ge::media::ff

#endif
