#ifndef GE_SRC_MEDIA_OPERATORS_INTERNAL_H_
#define GE_SRC_MEDIA_OPERATORS_INTERNAL_H_

#include <string>
#include <string_view>

#include <ge/cpp/capability.h>
#include <ge/cpp/operator.h>
#include <ge/media/device_buffer.h>
#include <ge/media/frame.h>
#include <ge/media/operators.h>

#include "media/ffmpeg.h"
#include "media/frame_internal.h"

namespace ge::media {

[[nodiscard]] const FormatSet& DecodedPixelFormats();
[[nodiscard]] const FormatSet& EncoderPixelFormats();

[[nodiscard]] const AVCodec* FindVideoEncoder(std::string_view codec, CodecBackend backend);
[[nodiscard]] Status RejectNonSoftwareBackend(const JsonValue& options, std::string_view node);

// Common per-node context every operator keeps from Open.
struct NodeContext {
  std::string external_id;
  NodeId node_id = 0;
  SessionId session_id = 0;

  void Capture(const OpenRequest& r) {
    external_id = std::string(r.external_id);
    node_id = r.node_id;
    session_id = r.session_id;
  }
  [[nodiscard]] std::string Prefix() const { return "node '" + external_id + "': "; }
};

// Builds a data packet around a media payload.
[[nodiscard]] inline Packet MakePacket(BufferRef payload, TypeTagId tag, PacketSeq seq, std::int64_t pts_ns,
                                       std::int64_t dts_ns, std::uint32_t flags = 0) {
  Packet p;
  p.header.seq = seq;
  p.header.pts_ns = pts_ns;
  p.header.dts_ns = dts_ns;
  p.header.flags = flags;
  p.header.type_tag = tag;
  p.payload = std::move(payload);
  return p;
}

[[nodiscard]] inline TypeTagId Tag(std::string_view name) { return TypeTagRegistry::Global().Intern(name); }

}  // namespace ge::media

#endif
