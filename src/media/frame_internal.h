#ifndef GE_SRC_MEDIA_FRAME_INTERNAL_H_
#define GE_SRC_MEDIA_FRAME_INTERNAL_H_

// FFmpeg-typed side of include/ge/media/frame.h.

#include <ge/media/frame.h>

#include "media/ffmpeg.h"

namespace ge::media {

// Takes ownership of one reference (the frame/packet is freed when the
// last BufferRef drops).
[[nodiscard]] BufferRef WrapFrame(ff::FramePtr frame);
[[nodiscard]] BufferRef WrapPacket(ff::PacketPtr packet);

// Read-only views; nullptr when the payload is not of that class.
[[nodiscard]] const AVFrame* FrameOf(const Packet& p) noexcept;
[[nodiscard]] const AVPacket* PacketOf(const Packet& p) noexcept;

// Format derivation.
[[nodiscard]] MediaFormat FormatOfFrame(const AVFrame& f, MediaKind kind, AVRational time_base);
[[nodiscard]] MediaFormat FormatOfCodecPar(const AVCodecParameters& par, AVRational time_base);

// codecpar <-> JSON (extradata base64) for lazily opened decoders/muxers.
[[nodiscard]] JsonValue CodecParToJson(const AVCodecParameters& par);
[[nodiscard]] Status CodecParFromJson(const JsonValue& v, AVCodecParameters* out);

[[nodiscard]] std::string Base64Encode(const std::uint8_t* data, std::size_t size);
[[nodiscard]] std::string Base64Decode(std::string_view text);

[[nodiscard]] inline AVRational ToAv(Rational r) noexcept { return AVRational{r.num, r.den}; }
[[nodiscard]] inline Rational FromAv(AVRational r) noexcept { return Rational{r.num, r.den}; }

}  // namespace ge::media

#endif
