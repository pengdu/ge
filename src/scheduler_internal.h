#ifndef GE_SRC_SCHEDULER_INTERNAL_H_
#define GE_SRC_SCHEDULER_INTERNAL_H_

// Private pieces shared by the scheduler translation units
// (scheduler.cpp, scheduler_invoke.cpp, scheduler_backpressure.cpp,
// scheduler_retire.cpp). State machine: docs/16-调度器状态机.md.

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
    Result<PacketRef> shared = PacketRouter::Prepare(topo_, node_, port, std::move(packet), pv_);
    if (!shared.ok()) return shared.status();
    if (!*shared) return Status::Ok();
    const std::vector<RouteEntry>* routes = topo_.RoutesFor(node_.id(), port);
    EmitReport report;
    for (const RouteEntry& r : *routes) {
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
  void Publish(std::string_view type, Severity severity, JsonValue detail) override {
    if (!active_ || !s_.events_.on_operator_event) return;
    s_.events_.on_operator_event(node_, std::string(type), severity, std::move(detail));
  }
  void Deactivate() noexcept { active_ = false; }

 private:
  Scheduler& s_;
  NodeRuntime& node_;
  const RuntimeTopology& topo_;
  ParameterVersion pv_;
  bool active_ = true;
};

}  // namespace ge

#endif
