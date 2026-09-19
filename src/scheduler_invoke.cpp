// Scheduler: one executor turn of a node (docs/16 §3 "Invoke 决策树").
//   Invoke / InvokeBody gate checks -> RunSource | RunProcess | RunAsync,
//   ParkAsync / OnAsyncIdle / OnAsyncNodeFailed, FinishNode (flush + EOS).
#include <ge/cpp/scheduler.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>

#include "scheduler_internal.h"

namespace ge {

void Scheduler::Invoke(NodeRuntime& node) {
  node.EnterBusy();
  const std::shared_ptr<const RuntimeTopology> topo = node.topology();
  InvokeBody(node, topo);
  node.ExitBusy();
  // Fast retire/stop while we were busy: TryCloseCancelled skipped us
  // (busy) so we close here; if it ran after ExitBusy the state is already
  // closed and this is a no-op.
  if (node.cancelled()) TryCloseCancelled(node);
}

void Scheduler::InvokeBody(NodeRuntime& node, const std::shared_ptr<const RuntimeTopology>& topo) {
  const NodeState s = node.state();
  if (s != NodeState::kReady && s != NodeState::kRunning) {
    node.CancelSchedule();
    return;
  }
  if (fast_stop_.load(std::memory_order_acquire) || node.cancelled()) {
    node.CancelSchedule();
    return;  // Invoke() closes after ExitBusy
  }

  if (node.held()) {
    node.CancelSchedule();  // Publish re-marks after Release()
    return;
  }
  if (!topo) {
    node.CancelSchedule();
    return;
  }
  if (HasParked(node) && !RetryParked(node)) {
    node.CancelSchedule();  // consumer's Pop re-marks us (12 §6.2)
    // Lost-wakeup guard: a consumer's Pop that drained our parked output
    // while we still held the slot could not re-mark us.
    if (!HasParked(node) && !node.held()) MarkReady(node);
    return;
  }
  if (OutputsBlocked(node, *topo)) {
    node.CancelSchedule();  // consumer's Pop re-marks us (12 §6.2)
    // Lost-wakeup guard: same race on a slot freed while we held the slot.
    if (!OutputsBlocked(node, *topo) && !node.held()) MarkReady(node);
    return;
  }
  if (node.is_source()) {
    RunSource(node, *topo);
  } else {
    InputBinding* binding = topo->InputsFor(node.id());
    if (binding == nullptr) {
      node.CancelSchedule();
      return;
    }
    if (node.is_async()) {
      RunAsync(node, topo, *binding);
    } else {
      RunProcess(node, *topo, *binding);
    }
  }
}

namespace {

// Moves an acquired batch into a request: events first, then data, with a
// parallel port list (12 §4.5; on_event arrives with P7).
template <typename Ports>
void MoveBatchInto(InputBatch& batch, Ports& ports, std::vector<PacketRef>& inputs) {
  for (PacketRef& ev : batch.events) {
    ports.emplace_back();
    inputs.push_back(std::move(ev));
  }
  for (std::size_t i = 0; i < batch.packets.size(); ++i) {
    ports.emplace_back(batch.ports[i]);
    inputs.push_back(std::move(batch.packets[i]));
  }
}

PacketSeq FirstSeq(const InputBatch& batch) {
  return batch.packets.empty() ? 0 : batch.packets.front()->header.seq;
}

}  // namespace

// Pops a batch and wakes producers whose block edge just freed a slot
// (parked data first, then a draining producer's EOS, else a plain re-mark).
std::optional<InputBatch> Scheduler::AcquireBatch(InputBinding& binding) {
  std::optional<InputBatch> batch = binding.TryAcquire();
  for (const EdgeChannelRef& e : binding.Edges()) {
    if (!e->ClearBackpressure()) continue;
    if (const std::shared_ptr<NodeRuntime> up = e->producer()) {
      if (HasParked(*up)) (void)RetryParked(*up);
      if (up->state() == NodeState::kDraining) {
        RetryBlocked(*up);
      } else {
        MarkReady(*up);
      }
    }
  }
  // A pop may have retired a drained predecessor edge or freed a slot for a
  // pending EOS injection (12 §7.7 B4).
  if (retire_pending_.load(std::memory_order_acquire) != 0) Tick();
  return batch;
}

// One synchronous Operator::Process call with timing and the failure
// protocol: Fail() before EndInvoke() so no further invocation is admitted;
// an error from a cancelled node is not a failure (12 §7.7). Returns the
// result only when the node may go on.
std::optional<ProcessResult> Scheduler::RunProcessCall(NodeRuntime& node, ProcessRequest& req, Sink& sink) {
  const auto start = std::chrono::steady_clock::now();
  auto r = node.op().Process(req);
  sink.Deactivate();
  node.metrics().process_ns_total.fetch_add(
      static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
      std::memory_order_relaxed);
  if (!r.ok() && node.cancelled()) {
    node.EndInvoke();
    return std::nullopt;
  }
  if (!r.ok()) node.Fail(r.status());
  node.EndInvoke();
  if (!r.ok()) {
    req.inputs.clear();  // release before OnNodeDone may signal all-closed
    if (events_.on_node_failed) events_.on_node_failed(node, r.status());
    OnNodeDone(node);
    return std::nullopt;
  }
  if (node.cancelled()) return std::nullopt;
  return *r;
}

void Scheduler::RunSource(NodeRuntime& node, const RuntimeTopology& topo) {
  if (paused_.load(std::memory_order_acquire) && !node.retiring()) {
    node.CancelSchedule();
    return;
  }
  if (stopping_.load(std::memory_order_acquire) || node.retiring()) {
    node.CancelSchedule();
    if (node.TryEnterDraining()) FinishNode(node);
    return;
  }
  const auto params = node.parameters().Current();
  node.BeginInvoke();
  Sink sink(*this, node, topo, params->version);
  ProcessRequest req;
  req.session_id = topo.session_id();
  req.topology_version = topo.version();
  req.parameter_version = params->version;
  req.parameters = &params->values;
  req.sink = &sink;
  req.events = &sink;
  const std::optional<ProcessResult> r = RunProcessCall(node, req, sink);
  if (!r) return;
  if (*r == ProcessResult::kExhausted) {
    if (node.TryEnterDraining()) FinishNode(node);
    return;
  }
  // Self-driving source: re-mark unless some output edge is full (the
  // consumer's Pop re-marks us via ClearBackpressure). A held node is
  // re-marked by Publish once released.
  if (node.held()) return;
  if (!OutputsBlocked(node, topo)) MarkReady(node);
}

void Scheduler::RunProcess(NodeRuntime& node, const RuntimeTopology& topo, InputBinding& binding) {
  std::optional<InputBatch> batch = AcquireBatch(binding);
  if (!batch) {
    node.CancelSchedule();
    if (binding.InputEnded() && node.TryEnterDraining()) {
      FinishNode(node);
      return;
    }
    // Lost-wakeup guard: a producer whose MarkReady lost to our slot must be
    // retried now that the slot is free.
    if (binding.MayBeReady() && !node.held()) MarkReady(node);
    return;
  }
  const PacketSeq first_seq = FirstSeq(*batch);
  (void)node.parameters().SwitchAtPacketBoundary(first_seq);
  const auto params = node.parameters().Current();
  node.BeginInvoke();
  node.NoteSeq(first_seq);
  Sink sink(*this, node, topo, params->version);
  ProcessRequest req;
  req.session_id = topo.session_id();
  req.topology_version = topo.version();
  req.parameter_version = params->version;
  req.parameters = &params->values;
  req.sink = &sink;
  req.events = &sink;
  MoveBatchInto(*batch, req.input_ports, req.inputs);
  node.metrics().packets_in.fetch_add(req.inputs.size(), std::memory_order_relaxed);
  if (!RunProcessCall(node, req, sink)) return;
  // The batch is consumed: release the payloads before anything that may
  // signal "all closed" (a leak check right after Stop would see them).
  req.inputs.clear();
  if (node.held()) return;  // Publish re-marks after Release()
  if (binding.MayBeReady()) {
    MarkReady(node);
  } else if (binding.InputEnded() && node.TryEnterDraining()) {
    FinishNode(node);
  }
}

// 12 §4.2: async nodes hand their batch to the AsyncRuntime and return at
// once; the executor thread is never blocked on the backend. Completion
// delivery (PacketRouter.Emit) happens on the runtime's consumer thread.
void Scheduler::RunAsync(NodeRuntime& node, const std::shared_ptr<const RuntimeTopology>& topo,
                         InputBinding& binding) {
  if (node.async_pending() >= node.async_max_in_flight()) {
    node.CancelSchedule();
    ParkAsync(node, false);
    return;
  }
  std::optional<InputBatch> batch = AcquireBatch(binding);
  if (!batch) {
    node.CancelSchedule();
    if (binding.InputEnded()) {
      // 12 §4.4 step 7: wait for in-flight completions, then flush.
      if (node.TryEnterDraining()) {
        FinishNode(node);
      } else if (node.async_pending() != 0) {
        ParkAsync(node, true);  // EndAsync -> OnAsyncIdle -> MarkReady
      }
      return;
    }
    if (binding.MayBeReady() && !node.held()) MarkReady(node);
    return;
  }
  if (!async_.submit) {
    node.CancelSchedule();
    Status st = Status::Internal("node '" + node.external_id() + "' is async but no AsyncRuntime is attached");
    node.Fail(st);
    if (events_.on_node_failed) events_.on_node_failed(node, st);
    OnNodeDone(node);
    return;
  }
  const PacketSeq first_seq = FirstSeq(*batch);
  (void)node.parameters().SwitchAtPacketBoundary(first_seq);
  const auto params = node.parameters().Current();
  node.BeginInvoke();
  node.NoteSeq(first_seq);
  AsyncDispatch::Request req;
  req.node = node.shared_from_this();
  req.topology = topo;
  req.parameters = params;
  req.packet_seq = first_seq;
  MoveBatchInto(*batch, req.input_ports, req.inputs);
  node.metrics().packets_in.fetch_add(req.inputs.size(), std::memory_order_relaxed);
  node.BeginAsync();
  async_.submit(std::move(req));
  node.EndInvoke();
  if (node.cancelled() || node.held()) return;
  if (binding.MayBeReady()) {
    MarkReady(node);
  } else if (binding.InputEnded()) {
    MarkReady(node);  // observe input end on the next turn
  }
}

// Parks the node until a completion frees a slot. The re-check closes the
// window where the last completion ran between our decision and the state
// switch (it saw ready and did not re-mark).
void Scheduler::ParkAsync(NodeRuntime& node, bool until_zero) {
  node.EnterAsyncPending();
  const std::uint32_t pending = node.async_pending();
  const bool freed = until_zero ? pending == 0 : pending < node.async_max_in_flight();
  if (freed && node.TryLeaveAsyncPending()) {
    if (!node.held()) MarkReady(node);
  }
}

void Scheduler::OnAsyncIdle(NodeRuntime& node) {
  if (node.cancelled()) {
    TryCloseCancelled(node);
    return;
  }
  const NodeState s = node.state();
  if (s != NodeState::kReady && s != NodeState::kRunning) return;
  if (!node.held()) MarkReady(node);
}

void Scheduler::OnAsyncNodeFailed(NodeRuntime& node, const Status& status) {
  if (events_.on_node_failed) events_.on_node_failed(node, status);
  OnNodeDone(node);
}

void Scheduler::AfterEmit(const EmitReport& report) {
  for (NodeRuntime* target : report.targets) MarkReady(*target);
}

// 12 §4.4 steps 3–4 (+6 for sinks). Called with node in draining state and
// in_flight == 0, on the thread that observed input end.
void Scheduler::FinishNode(NodeRuntime& node) {
  const std::shared_ptr<const RuntimeTopology> topo = node.topology();
  const auto params = node.parameters().Current();
  if (topo && !node.cancelled()) {
    Sink sink(*this, node, *topo, params->version);
    ProcessRequest req;
    req.session_id = topo->session_id();
    req.topology_version = topo->version();
    req.parameter_version = params->version;
    req.flags = GE_PROCESS_FLAG_FLUSH;
    req.parameters = &params->values;
    req.sink = &sink;
  req.events = &sink;
    node.metrics().invocations.fetch_add(1, std::memory_order_relaxed);
    auto r = node.op().Process(req);
    sink.Deactivate();
    if (!r.ok() && !node.cancelled()) {
      node.Fail(r.status());
      if (events_.on_node_failed) events_.on_node_failed(node, r.status());
    }
  }
  {
    std::lock_guard lock(retry_mutex_);
    auto& ports = eos_retry_[&node];
    if (topo) {
      for (const std::string& p : topo->OutputPorts(node.id())) ports.insert(p);
    }
  }
  RetryBlocked(node);
}
}  // namespace ge
