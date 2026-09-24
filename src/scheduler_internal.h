#ifndef GE_SRC_SCHEDULER_INTERNAL_H_
#define GE_SRC_SCHEDULER_INTERNAL_H_

// Private pieces shared by the scheduler translation units
// (scheduler.cpp, scheduler_invoke.cpp, scheduler_backpressure.cpp,
// scheduler_retire.cpp). State machine: docs/16-调度器状态机.md.

#include <algorithm>
#include <optional>
#include <string_view>
#include <vector>

#include <ge/cpp/input_binding.h>
#include <ge/cpp/scheduler.h>

namespace ge {

// EmitSink/EventSink handed to Operator::Process for one invocation. Emit
// pushes to every route of the port; a full block-policy edge parks the
// packet (12 §6.2, docs/16 §4).
class Scheduler::Sink final : public EmitSink, public EventSink {
 public:
  Sink(Scheduler& s, NodeRuntime& node, const RuntimeTopology& topo, ParameterVersion pv)
      : s_(s), node_(node), topo_(topo), pv_(pv) {}
  Status Emit(std::string_view port, Packet packet) override {
    if (!active_) return Status::InvalidArgument("emit outside of process");
    node_.NoteSeq(packet.header.seq);
    // EVT-4: mirror a staged format event, if any, immediately before the
    // keyframe it binds to. Both packets go through the same per-edge
    // decision (PushOnRoutes), so a full block edge parks them in FIFO order
    // and the downstream never sees the keyframe first.
    if (packet.header.flags & GE_PACKET_FLAG_KEYFRAME) {
      const std::vector<RouteEntry>* routes = topo_.RoutesFor(node_.id(), port);
      // A keyframe only consumes a candidate when this port actually carries
      // a video route: a data keyframe on an opaque output must not swallow
      // an event bound to a video keyframe that has not been emitted yet, and
      // a sibling video port must not replay it.
      const bool video_port =
          routes != nullptr && std::any_of(routes->begin(), routes->end(),
                                           [](const RouteEntry& r) { return IsVideoRoute(r.contract); });
      if (video_port && !pending_.empty()) {
        const auto it = std::find_if(
            pending_.begin(), pending_.end(),
            [&](const PendingFormatEvent& p) { return p.seq == packet.header.seq; });
        if (it != pending_.end()) {
          const JsonValue detail = it->detail;
          pending_.erase(it);  // one event Packet per keyframe, never two
          if (const Status s = PushOnRoutes(port, MakeFormatEventPacket(packet, kFormatEventType, detail),
                                            /*video_only=*/true);
              !s.ok()) {
            return s;
          }
        }
      }
    }
    return PushOnRoutes(port, std::move(packet), /*video_only=*/false);
  }
  void Publish(std::string_view type, Severity severity, JsonValue detail) override {
    if (!active_) return;
    // EVT-4: a format event that names the keyframe it belongs to is also
    // mirrored into the data plane. Staging happens here, on the same thread
    // and inside the same Process call that will emit the keyframe; an event
    // published outside a call (open/close) never reaches this function at
    // all, so no scope bookkeeping is needed.
    const std::optional<PacketSeq> seq = FormatEventMatchSeq(detail);
    if (type == kFormatEventType && seq.has_value()) {
      pending_.push_back(PendingFormatEvent{*seq, detail});
    }
    // The side-band publication is unchanged and must stay exactly one per
    // call: it is the observation contract (12 §4.5), not a data-plane
    // delivery -- so it happens with or without a mirrorable candidate.
    if (!s_.events_.on_operator_event) return;
    s_.events_.on_operator_event(node_, std::string(type), severity, std::move(detail));
  }
  void Deactivate() noexcept { active_ = false; }
  // Records that a staged event never found its keyframe. Called once per
  // leftover when the call ends; the side-band copy already went out.
  ~Sink() {
    node_.metrics().format_events_unmirrored.fetch_add(pending_.size(), std::memory_order_relaxed);
  }

 private:
  struct PendingFormatEvent {
    PacketSeq seq;
    JsonValue detail;
  };

  // Pushes |packet| to every route of |port|. The keyframe and the mirrored
  // event must not drift apart, so this is the one loop both use; |video_only|
  // is the only difference between them (an event is a video-only mirror,
  // while the keyframe reaches every edge, video or not).
  Status PushOnRoutes(std::string_view port, Packet packet, bool video_only) {
    Result<PacketRef> shared = PacketRouter::Prepare(topo_, node_, port, std::move(packet), pv_);
    if (!shared.ok()) return shared.status();
    if (!*shared) return Status::Ok();  // unconnected output: not an error
    const std::vector<RouteEntry>* routes = topo_.RoutesFor(node_.id(), port);
    EmitReport report;
    for (const RouteEntry& r : *routes) {
      if (video_only && !IsVideoRoute(r.contract)) continue;  // audio/tensor/opaque legs get the keyframe only
      // Same-edge FIFO: once something is parked for an edge, later packets
      // queue behind it instead of overtaking.
      if (s_.HasParked(node_, *r.edge)) {
        s_.Park(node_, *r.edge, *shared);
        continue;
      }
      if (PacketRouter::PushOne(*r.edge, *shared, node_, &report) == PushOutcome::kWouldBlock) {
        s_.Park(node_, *r.edge, *shared);
      }
    }
    s_.AfterEmit(report);
    // Block policy (12 §6.2): a packet that hit a full edge is kept by the
    // engine and delivered once the consumer frees a slot, so from the
    // operator's point of view the emit succeeded (no retry, no loss). The
    // node is not invoked again until its parked output went through.
    return Status::Ok();
  }

  Scheduler& s_;
  NodeRuntime& node_;
  const RuntimeTopology& topo_;
  ParameterVersion pv_;
  std::vector<PendingFormatEvent> pending_;
  bool active_ = true;
};

}  // namespace ge

#endif
