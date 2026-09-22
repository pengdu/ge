#ifndef GE_MEDIA_FRAME_H_
#define GE_MEDIA_FRAME_H_

//
//   VideoFrame / AudioFrame  payload = one AVFrame*  (size = sizeof(AVFrame))
//   EncodedVideo / EncodedAudio payload = one AVPacket* (size = sizeof(AVPacket))
//
// The Buffer owns exactly one libav reference; consumers treat the frame
// as read-only and must av_frame_ref() if they need a private copy. The
// public header stays FFmpeg-free: the raw pointers are opaque here and are
// re-typed inside ge_media (src/media/frame_internal.h).
//
// Metadata: "format" carries a MediaFormat JSON (see below); encoded
// keyframes also carry "codecpar" (JSON with base64 extradata) so a lazily
// opened decoder/muxer can configure itself from the first packet alone.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <ge/cpp/json.h>
#include <ge/cpp/packet.h>
#include <ge/cpp/types.h>

namespace ge::media {

inline constexpr std::string_view kMetaFormat = "format";
inline constexpr std::string_view kMetaCodecPar = "codecpar";

// Builtin type tags used by the media operators.
inline constexpr std::string_view kTagVideoFrame = "VideoFrame";
inline constexpr std::string_view kTagAudioFrame = "AudioFrame";
inline constexpr std::string_view kTagEncodedVideo = "EncodedVideo";
inline constexpr std::string_view kTagEncodedAudio = "EncodedAudio";

enum class MediaKind : std::uint8_t { kVideo, kAudio };

// Rational as {num, den}.
struct Rational {
  std::int32_t num = 0;
  std::int32_t den = 1;
  friend bool operator==(const Rational&, const Rational&) = default;
};

// Serialisable description of a frame/packet stream; stored under the
// "format" metadata key so downstream nodes can react to changes without
// touching the payload.
struct MediaFormat {
  MediaKind kind = MediaKind::kVideo;
  // Video.
  std::string pixel_format;  // libav name, e.g. "yuv420p"
  std::int32_t width = 0;
  std::int32_t height = 0;
  Rational frame_rate;  // 0/1 when unknown
  Rational sample_aspect_ratio{1, 1};
  // Audio.
  std::string sample_format;  // e.g. "fltp"
  std::int32_t sample_rate = 0;
  std::int32_t channels = 0;
  std::string channel_layout;  // e.g. "stereo"
  // Both.
  Rational time_base{1, 1000000000};
  std::string codec;  // encoded streams only, e.g. "h264"

  [[nodiscard]] JsonValue ToJson() const;
  [[nodiscard]] static Result<MediaFormat> FromJson(const JsonValue& v);
  [[nodiscard]] std::string Serialize() const { return ToJson().Serialize(); }
  [[nodiscard]] static std::optional<MediaFormat> FromPacket(const Packet& p);
  friend bool operator==(const MediaFormat&, const MediaFormat&) = default;
};

// Attaches |format| (and optional codecpar JSON) to |packet| metadata.
void SetFormat(Packet* packet, const MediaFormat& format, const JsonValue* codecpar = nullptr);
[[nodiscard]] const std::string* CodecParJson(const Packet& p);

// Opaque payload access. Returns nullptr when the payload is not a media
// buffer of the requested class.
[[nodiscard]] bool IsFramePayload(const Buffer& b) noexcept;
[[nodiscard]] bool IsPacketPayload(const Buffer& b) noexcept;

// Convenience for tests/tools: pixel/sample format names understood by
// the operators (validated through libav at runtime).
[[nodiscard]] bool IsKnownPixelFormat(std::string_view name);
[[nodiscard]] bool IsKnownSampleFormat(std::string_view name);

}  // namespace ge::media

#endif
