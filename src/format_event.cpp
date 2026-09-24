#include <ge/cpp/input_binding.h>

#include <memory>
#include <string>

#include <ge/c/ge_abi.h>
#include <ge/cpp/packet.h>
#include <ge/cpp/types.h>

namespace ge {

bool IsVideoRoute(const ConnectionContract& contract) { return contract.video.has_value(); }

std::optional<PacketSeq> FormatEventMatchSeq(const JsonValue& detail) {
  // GetInteger() returns nullopt for a missing key and for a non-number, so
  // there is no separate presence check to get wrong.
  const std::optional<std::int64_t> n = detail.GetInteger(kFormatEventSeqKey);
  if (!n.has_value() || *n <= 0) return std::nullopt;
  return static_cast<PacketSeq>(*n);
}

Packet MakeFormatEventPacket(const Packet& keyframe, std::string_view, const JsonValue& detail) {
  Packet ev;
  ev.header.seq = keyframe.header.seq;
  ev.header.pts_ns = keyframe.header.pts_ns;
  ev.header.dts_ns = keyframe.header.dts_ns;
  ev.header.flags = GE_PACKET_FLAG_EVENT;
  ev.header.type_tag = TypeTagRegistry::Global().Intern(std::string(kFormatEventType));
  // Metadata stores string-valued entries, so the detail is flattened the
  // same way the C ABI path does it (src/plugin_operator.cpp:293).
  if (Result<Metadata> m = Metadata::FromJson(detail.Serialize()); m.ok()) {
    ev.metadata = std::make_shared<const Metadata>(std::move(*m));
  }
  ev.ingress_ns = keyframe.ingress_ns;
  return ev;
}

}  // namespace ge
