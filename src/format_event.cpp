#include <ge/cpp/input_binding.h>

#include <memory>
#include <string>

#include <ge/c/ge_abi.h>
#include <ge/cpp/packet.h>
#include <ge/cpp/types.h>

namespace ge {

bool IsVideoRoute(const ConnectionContract& contract) { return contract.video.has_value(); }

std::optional<PacketSeq> FormatEventMatchSeq(const JsonValue& detail) {
  // Only a JSON integer binds an event to a keyframe. GetInteger() would also
  // accept a kNumber (e.g. 4.9), whose as_integer() truncates -- and for a
  // value like 1e300 that cast is undefined behaviour. Find() + is_integer()
  // checks the type before any cast, so the value is always a real int64.
  // The parser classifies an integer *token* as kInteger (src/json.cpp:114),
  // so a producer's detail still matches after a ParseJson round trip.
  const JsonValue* v = detail.Find(kFormatEventSeqKey);
  if (v == nullptr || !v->is_integer()) return std::nullopt;
  const std::int64_t n = v->as_integer();
  if (n <= 0) return std::nullopt;
  return static_cast<PacketSeq>(n);
}

Packet MakeFormatEventPacket(const Packet& keyframe, std::string_view event_type,
                             const JsonValue& detail) {
  Packet ev;
  ev.header.seq = keyframe.header.seq;
  ev.header.pts_ns = keyframe.header.pts_ns;
  ev.header.dts_ns = keyframe.header.dts_ns;
  ev.header.flags = GE_PACKET_FLAG_EVENT;
  // Tag with the event name we were handed: deciding *which* events are
  // mirrored is the caller's rule (Task 3's Sink::Publish predicate), not the
  // builder's. Substituting kFormatEventType here would misreport a caller
  // that asked for a different type.
  ev.header.type_tag = TypeTagRegistry::Global().Intern(std::string(event_type));
  // Metadata stores string-valued entries, so the detail is flattened the
  // same way the C ABI path does it (src/plugin_operator.cpp:293).
  if (Result<Metadata> m = Metadata::FromJson(detail.Serialize()); m.ok()) {
    ev.metadata = std::make_shared<const Metadata>(std::move(*m));
  }
  ev.ingress_ns = keyframe.ingress_ns;
  return ev;
}

}  // namespace ge
