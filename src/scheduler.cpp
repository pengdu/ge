// Scheduler: lifecycle and node bookkeeping (docs/16 §2, §3).
//   construction / attach / detach, OpenNodes / AbandonCandidate,
//   Start / Pause / Resume / Stop, MarkReady (slot -> executor task),
//   CloseNode / TryCloseCancelled / OnNodeDone / CheckAllClosed.
// Invocation lives in scheduler_invoke.cpp, block-policy parking in
// scheduler_backpressure.cpp, Publish/Retire in scheduler_retire.cpp.
#include <ge/cpp/scheduler.h>

#include <algorithm>
#include <chrono>
#include <memory>

#include "scheduler_internal.h"

namespace ge {

Scheduler::Scheduler(std::shared_ptr<RuntimeTopology> topology, ExecutorPool& executor,
                     SchedulerEvents events)
    : topology_(std::move(topology)), executor_(executor), events_(std::move(events)) {
  const std::shared_ptr<RuntimeTopology> topo = topology_.load();
  for (const NodeRuntimeRef& n : topo->nodes()) n->SetTopology(topo);
  AttachNodes(topo->nodes());
}

Scheduler::~Scheduler() {
  alive_.store(false, std::memory_order_release);
  // Inline executor: nobody else runs (or drops) tasks queued after the
  // last host pump, so drain them here before waiting.
  if (executor_.cpu_threads() == 0) {
    while (pending_tasks_.load(std::memory_order_acquire) != 0 && executor_.RunPending()) {
    }
  }
  {
    std::unique_lock lock(closed_mutex_);
    closed_cv_.wait(lock, [this] { return pending_tasks_.load(std::memory_order_acquire) == 0; });
  }
  // Break node <-> topology cycles so operators (and plugin leases) are
  // released with the session even when nodes never reached closed.
  std::vector<std::shared_ptr<const RuntimeTopology>> topologies;
  topologies.push_back(topology());
  {
    std::lock_guard lock(retire_mutex_);
    for (const Retired& r : retired_) topologies.push_back(r.request.topology);
  }
  for (const auto& t : topologies) {
    if (!t) continue;
    for (const NodeRuntimeRef& n : t->nodes()) n->SetTopology(nullptr);
  }
  for (const NodeRuntimeRef& n : LiveNodes()) n->SetTopology(nullptr);
}

void Scheduler::AttachNodes(const std::vector<NodeRuntimeRef>& nodes) {
  std::lock_guard lock(live_mutex_);
  for (const NodeRuntimeRef& n : nodes) {
    const bool present = std::any_of(live_nodes_.begin(), live_nodes_.end(),
                                     [&](const NodeRuntimeRef& l) { return l == n; });
    if (present) continue;
    live_nodes_.push_back(n);
    live_count_.fetch_add(1, std::memory_order_acq_rel);
  }
}

bool Scheduler::Detach(NodeRuntime& node) {
  std::lock_guard lock(live_mutex_);
  const auto it = std::find_if(live_nodes_.begin(), live_nodes_.end(),
                               [&](const NodeRuntimeRef& l) { return l.get() == &node; });
  if (it == live_nodes_.end()) return false;
  live_nodes_.erase(it);
  live_count_.fetch_sub(1, std::memory_order_acq_rel);
  return true;
}

std::vector<NodeRuntimeRef> Scheduler::LiveNodes() const {
  std::lock_guard lock(live_mutex_);
  return live_nodes_;
}

Status Scheduler::OpenNodes(const RuntimeTopology& candidate) {
  std::vector<NodeRuntime*> opened;
  for (const std::string& id : candidate.topological_order()) {
    NodeRuntime* node = candidate.FindNode(id);
    if (node->state() != NodeState::kCreated) continue;  // carried over / warmed up
    OpenRequest req;
    req.session_id = candidate.session_id();
    req.topology_version = candidate.version();
    req.options = &node->options();
    req.node_id = node->id();
    req.external_id = node->external_id();
    // P6: negotiated contracts (12 §8.2) so builtin operators can pick the
    // agreed formats. Pointers stay valid while |candidate| is alive, i.e.
    // for the duration of Open.
    if (const InputBinding* binding = candidate.InputsFor(node->id())) {
      for (const InputPortBinding& b : binding->ports()) {
        if (b.edge) req.input_contracts.push_back({b.port, &b.edge->contract()});
      }
    }
    for (const std::string& port : candidate.OutputPorts(node->id())) {
      const auto* routes = candidate.RoutesFor(node->id(), port);
      if (routes != nullptr && !routes->empty()) {
        req.output_contracts.push_back({port, &routes->front().contract});
      }
    }
    if (Status s = node->Open(req); !s.ok()) {
      for (NodeRuntime* n : opened) {
        (void)n->Close(CloseRequest{candidate.session_id(), candidate.version(), true});
      }
      return s;
    }
    opened.push_back(node);
  }
  return Status::Ok();
}

void Scheduler::AbandonCandidate(const RuntimeTopology& candidate) {
  for (const NodeRuntimeRef& n : candidate.new_nodes()) {
    const NodeState s = n->state();
    if (s == NodeState::kClosed || s == NodeState::kFailed) continue;
    (void)n->Close(CloseRequest{candidate.session_id(), candidate.version(), true});
  }
  for (const EdgeChannelRef& e : candidate.new_edges()) e->MarkRetired(true);
}

void Scheduler::Start() {
  started_.store(true, std::memory_order_release);
  for (const NodeRuntimeRef& n : topology()->nodes()) {
    if (n->is_source()) MarkReady(*n);
  }
}

void Scheduler::Pause() { paused_.store(true, std::memory_order_release); }

void Scheduler::Resume() {
  paused_.store(false, std::memory_order_release);
  for (const NodeRuntimeRef& n : topology()->nodes()) {
    if (n->is_source()) MarkReady(*n);
  }
}

void Scheduler::Stop(bool fast) {
  const std::vector<NodeRuntimeRef> live = LiveNodes();
  if (!fast) {
    stopping_.store(true, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    for (const NodeRuntimeRef& n : live) {
      if (n->is_source()) MarkReady(*n);
    }
    Tick();
    return;
  }
  // Cancel before announcing the stop: a source observing stopping_ first
  // would take the drain path (EOS + graceful Close) instead of the fast one.
  fast_stop_.store(true, std::memory_order_release);
  for (const NodeRuntimeRef& n : live) n->MarkCancelled();
  stopping_.store(true, std::memory_order_release);
  {
    std::lock_guard lock(retire_mutex_);
    for (Retired& r : retired_) {
      if (!r.fast_applied) (void)ApplyFastLocked(r);
    }
  }
  for (const NodeRuntimeRef& n : live) {
    if (const auto topo = n->topology()) {
      for (const EdgeChannelRef& e : topo->edges()) e->MarkRetired(true);
    }
  }
  for (const EdgeChannelRef& e : topology()->edges()) e->MarkRetired(true);
  // Running process calls are not interrupted (12 §7.7): a node that is
  // mid-invoke closes itself when the call returns (InvokeBody checks
  // cancelled()); everything idle is closed here. Non-blocking so it can be
  // called from an executor thread (session failure).
  for (const NodeRuntimeRef& n : live) TryCloseCancelled(*n);
  Tick();
  CheckAllClosed();
}

bool Scheduler::WaitClosed(std::uint64_t timeout_ms) {
  std::unique_lock lock(closed_mutex_);
  return closed_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this] { return closed_signalled_; });
}

void Scheduler::MarkReady(NodeRuntime& node) {
  if (!started_.load(std::memory_order_acquire) || fast_stop_.load(std::memory_order_acquire)) return;
  if (!alive_.load(std::memory_order_acquire)) return;
  if (!node.TrySchedule()) return;
  // The pending count is released when the task object dies, so tasks the
  // executor drops on Stop() (never run) still let ~Scheduler proceed. The
  // NodeRuntime is kept alive by the shared_ptr captured here.
  struct PendingGuard {
    PendingGuard(Scheduler* s_, NodeRuntimeRef node_) : s(s_), node(std::move(node_)) {
      s->pending_tasks_.fetch_add(1, std::memory_order_acq_rel);
    }
    PendingGuard(const PendingGuard&) = delete;
    PendingGuard& operator=(const PendingGuard&) = delete;
    Scheduler* s;
    NodeRuntimeRef node;
    bool ran = false;
    ~PendingGuard() {
      if (!ran) node->CancelSchedule();
      // Decrement under the mutex: ~Scheduler's wait may otherwise observe
      // zero (spurious wakeup or predicate poll) and destroy the mutex while
      // we are about to lock it.
      std::lock_guard lock(s->closed_mutex_);
      if (s->pending_tasks_.fetch_sub(1, std::memory_order_acq_rel) == 1) s->closed_cv_.notify_all();
    }
  };
  auto guard = std::make_shared<PendingGuard>(this, node.shared_from_this());
  executor_.SubmitCpu([guard] {
    guard->ran = true;
    guard->s->Invoke(*guard->node);
  });
}

void Scheduler::CloseNode(NodeRuntime& node, bool fast) {
  const std::shared_ptr<const RuntimeTopology> topo = node.topology();
  const SessionId sid = topo ? topo->session_id() : 0;
  const TopologyVersion tv = topo ? topo->version() : 0;
  if (node.state() != NodeState::kCreated) node.EnterClosing();
  (void)node.Close(CloseRequest{sid, tv, fast});
}

bool Scheduler::TryCloseCancelled(NodeRuntime& node) {
  // An invoke task on the stack closes the node itself when it returns.
  if (node.busy()) return false;
  const NodeState s = node.state();
  if (s == NodeState::kClosed || s == NodeState::kFailed) {
    // Already terminal (e.g. warm-up rollback) but possibly still attached.
    OnNodeDone(node);
    return false;
  }
  if (s == NodeState::kClosing) return false;
  // ASY-9: in-flight async requests are awaited (bounded by
  // max_inference_ms); the AsyncRuntime re-enters here via OnAsyncIdle.
  if (node.async_pending() != 0) return false;
  if (s == NodeState::kAsyncPending && !node.TryLeaveAsyncPending()) return false;
  if (s == NodeState::kDraining) {
    // Waiting on blocked EOS pushes: they are cancelled now, so this closes.
    RetryBlocked(node);
    const NodeState now = node.state();
    return now == NodeState::kClosed || now == NodeState::kFailed;
  }
  // Exactly one closer: ready/running -> draining CAS, or created/opening
  // which nobody else touches.
  if (!node.TryEnterDraining()) {
    if (node.state() != NodeState::kCreated) return false;
  }
  CloseNode(node, true);
  OnNodeDone(node);
  return true;
}

void Scheduler::OnNodeDone(NodeRuntime& node) {
  if (!Detach(node)) return;
  DropParked(node);
  // Closed/failed: drop the routing snapshot (12 §7.7 "removed nodes keep
  // Vn until closed") which also breaks the node <-> topology cycle.
  node.SetTopology(nullptr);
  CheckAllClosed();
  if (retire_pending_.load(std::memory_order_acquire) != 0) Tick();
}

void Scheduler::CheckAllClosed() {
  if (live_count_.load(std::memory_order_acquire) != 0) return;
  bool expected = false;
  if (!all_closed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
  // Callback runs before waiters wake so observers see a consistent state.
  if (events_.on_all_closed) events_.on_all_closed();
  std::lock_guard lock(closed_mutex_);
  closed_signalled_ = true;
  closed_cv_.notify_all();
}
}  // namespace ge
