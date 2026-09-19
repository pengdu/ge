// Scheduler: topology swap and retirement (docs/16 §5, 12 §7.2 B1–B5, §7.7).
//   Publish (hold kept producers, swap, EOS markers), RetryInjections,
//   DrainComplete / ApplyFastLocked / CompleteRetired, Tick (watchdog).
#include <ge/cpp/scheduler.h>

#include <algorithm>
#include <chrono>
#include <memory>

#include "scheduler_internal.h"

namespace ge {

// ---------------------------------------------------------------------------
// Publish / Retire (12 §7.2 B1–B5, §7.7)
// ---------------------------------------------------------------------------

void Scheduler::WaitQuiescent(NodeRuntime& node) {
  // Bounded by one in-flight process call per slot (MUT-3): nothing new is
  // admitted while held. Inline executors are drained by the caller.
  while (!node.quiescent()) {
    if (executor_.cpu_threads() == 0) {
      (void)executor_.RunOne();
    } else {
      std::this_thread::yield();
    }
  }
}

Status Scheduler::Publish(std::shared_ptr<RuntimeTopology> next, RetireRequest retire) {
  if (stopping_.load(std::memory_order_acquire)) {
    return Status::Cancelled("session stopping: topology not published");
  }
  const std::shared_ptr<RuntimeTopology> prev = topology();
  retire.topology = prev;

  // Classify Vn's nodes/edges.
  std::vector<NodeRuntimeRef> kept, removed;
  for (const NodeRuntimeRef& n : prev->nodes()) {
    const bool in_next = std::any_of(next->nodes().begin(), next->nodes().end(),
                                     [&](const NodeRuntimeRef& m) { return m == n; });
    (in_next ? kept : removed).push_back(n);
  }
  std::vector<EdgeChannelRef> removed_edges;
  for (const EdgeChannelRef& e : prev->edges()) {
    const bool in_next = std::any_of(next->edges().begin(), next->edges().end(),
                                     [&](const EdgeChannelRef& m) { return m == e; });
    if (!in_next) removed_edges.push_back(e);
  }
  retire.removed_nodes = removed;
  retire.removed_edges = removed_edges;

  // Kept producers of removed edges: hold, wait for their in-flight call,
  // then rebind. Their old output edge gets an EOS marker so the consumer
  // (kept or new) knows the old path is complete.
  std::vector<NodeRuntimeRef> held;
  for (const EdgeChannelRef& e : removed_edges) {
    const std::shared_ptr<NodeRuntime> p = e->producer();
    if (!p) continue;
    const bool is_kept = std::any_of(kept.begin(), kept.end(),
                                     [&](const NodeRuntimeRef& k) { return k == p; });
    if (!is_kept) continue;
    if (std::find(held.begin(), held.end(), p) == held.end()) {
      p->Hold();
      held.push_back(p);
    }
  }
  for (const NodeRuntimeRef& n : held) WaitQuiescent(*n);

  // B1: swap. New nodes route through Vn+1 from their first invocation;
  // kept nodes switch now; removed nodes keep Vn (12 §7.7).
  next->ApplyRebinds();
  for (const NodeRuntimeRef& n : next->nodes()) n->SetTopology(next);
  topology_.store(next);
  AttachNodes(next->new_nodes());
  // Stop() raced with the swap: the new nodes are attached now, so the
  // stop's EOS/cancel sweep may have missed them. Re-run the sweep on the
  // new topology; both are idempotent.
  if (stopping_.load(std::memory_order_acquire)) {
    if (fast_stop_.load(std::memory_order_acquire)) {
      for (const NodeRuntimeRef& n : next->new_nodes()) n->MarkCancelled();
      for (const EdgeChannelRef& e : next->edges()) e->MarkRetired(true);
    }
  }

  // B3: removed edges of kept producers get an EOS marker right away (their
  // producer will never write to them again) and go draining; removed edges
  // whose producer is also removed stay writable until that producer
  // finishes (its in-flight packets keep the old chain, 12 §7.3/§7.4) and
  // receive the producer's own EOS.
  const auto deadline = std::chrono::steady_clock::now() + retire.drain_timeout;
  Retired entry{std::move(retire), deadline, false, false};
  {
    std::lock_guard lock(retire_mutex_);
    if (entry.request.policy == RemovePolicy::kFast) {
      (void)ApplyFastLocked(entry);
    } else {
      for (const NodeRuntimeRef& n : removed) n->MarkRetiring();
      for (const EdgeChannelRef& e : removed_edges) {
        const std::shared_ptr<NodeRuntime> p = e->producer();
        const bool producer_removed =
            p && std::any_of(removed.begin(), removed.end(),
                             [&](const NodeRuntimeRef& r) { return r == p; });
        if (producer_removed) continue;
        e->MarkDraining();
        const TypeTagId tag = TypeTagRegistry::Global().Intern(e->contract().logical_type);
        Packet eos = Packet::Eos(tag, prev->version(), 0, p ? p->last_seq() : 0);
        pending_inject_.push_back({e, std::make_shared<const Packet>(std::move(eos))});
      }
    }
    retired_.push_back(std::move(entry));
    retire_pending_.store(static_cast<std::uint32_t>(retired_.size()), std::memory_order_release);
  }
  RetryInjections();

  // Release held producers and kick everything that may have work.
  for (const NodeRuntimeRef& n : held) n->Release();
  if (stopping_.load(std::memory_order_acquire)) {
    if (fast_stop_.load(std::memory_order_acquire)) {
      for (const NodeRuntimeRef& n : next->new_nodes()) TryCloseCancelled(*n);
    } else {
      for (const NodeRuntimeRef& n : next->nodes()) {
        if (n->is_source()) MarkReady(*n);
      }
    }
  }
  if (started_.load(std::memory_order_acquire)) {
    for (const NodeRuntimeRef& n : next->nodes()) {
      if (n->is_source()) {
        MarkReady(*n);
      } else if (const InputBinding* b = next->InputsFor(n->id()); b && b->MayBeReady()) {
        MarkReady(*n);
      }
    }
    for (const NodeRuntimeRef& n : removed) {
      if (n->is_source()) {
        MarkReady(*n);  // retiring source finishes on its next turn
      } else if (const InputBinding* b = prev->InputsFor(n->id()); b && b->MayBeReady()) {
        MarkReady(*n);
      }
    }
  }
  Tick();
  return Status::Ok();
}

void Scheduler::RetryInjections() {
  std::vector<NodeRuntime*> wake;
  {
    std::lock_guard lock(retire_mutex_);
    for (auto it = pending_inject_.begin(); it != pending_inject_.end();) {
      const PushOutcome o = it->edge->InjectEos(it->eos);
      if (o == PushOutcome::kWouldBlock) {
        ++it;
        continue;
      }
      if (o == PushOutcome::kAccepted) {
        if (const std::shared_ptr<NodeRuntime> c = it->edge->consumer()) wake.push_back(c.get());
      }
      it = pending_inject_.erase(it);
    }
  }
  for (NodeRuntime* n : wake) MarkReady(*n);
}

bool Scheduler::DrainComplete(const Retired& r) const {
  const auto finished = [](const NodeRuntime& n) {
    const NodeState s = n.state();
    return s == NodeState::kClosed || s == NodeState::kFailed;
  };
  const auto is_removed = [&](const std::shared_ptr<NodeRuntime>& n) {
    return std::any_of(r.request.removed_nodes.begin(), r.request.removed_nodes.end(),
                       [&](const NodeRuntimeRef& m) { return m == n; });
  };
  for (const NodeRuntimeRef& n : r.request.removed_nodes) {
    if (!finished(*n)) return false;
  }
  for (const EdgeChannelRef& e : r.request.removed_edges) {
    if (e->state() == EdgeState::kRetired) continue;
    if (!e->empty()) return false;
    const bool pending = std::any_of(pending_inject_.begin(), pending_inject_.end(),
                                     [&](const PendingInject& q) { return q.edge == e; });
    if (pending) return false;
    const std::shared_ptr<NodeRuntime> p = e->producer();
    const std::shared_ptr<NodeRuntime> c = e->consumer();
    // Removed producer: done once it is closed (its EOS, if any, was popped).
    if (p && is_removed(p)) {
      if (!finished(*p)) return false;
      continue;
    }
    // Kept producer: it never writes again and its marker was injected and
    // popped (queue empty). A removed consumer must still be closed.
    if (c && is_removed(c) && !finished(*c)) return false;
  }
  return true;
}

std::vector<NodeRuntimeRef> Scheduler::ApplyFastLocked(Retired& r) {
  r.fast_applied = true;
  for (const NodeRuntimeRef& n : r.request.removed_nodes) n->MarkCancelled();
  for (const EdgeChannelRef& e : r.request.removed_edges) e->MarkRetired(true);
  std::erase_if(pending_inject_, [&](const PendingInject& p) {
    return std::any_of(r.request.removed_edges.begin(), r.request.removed_edges.end(),
                       [&](const EdgeChannelRef& e) { return e == p.edge; });
  });
  return r.request.removed_nodes;
}

bool Scheduler::CompleteRetired() {
  std::vector<Retired> done;
  std::vector<NodeRuntimeRef> to_close;
  std::vector<TopologyVersion> timed_out;
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard lock(retire_mutex_);
    for (auto it = retired_.begin(); it != retired_.end();) {
      Retired& r = *it;
      if (!r.fast_applied && now >= r.deadline && !DrainComplete(r)) {
        r.timed_out = true;
        auto nodes = ApplyFastLocked(r);
        to_close.insert(to_close.end(), nodes.begin(), nodes.end());
        timed_out.push_back(r.request.topology->version());
      }
      if (r.fast_applied) {
        // Fast: edges are retired; nodes close once their running call ends.
        for (const EdgeChannelRef& e : r.request.removed_edges) e->MarkRetired(true);
        bool nodes_done = true;
        for (const NodeRuntimeRef& n : r.request.removed_nodes) {
          const NodeState s = n->state();
          if (s != NodeState::kClosed && s != NodeState::kFailed) {
            nodes_done = false;
            to_close.push_back(n);
          }
        }
        if (!nodes_done) {
          ++it;
          continue;
        }
      } else if (!DrainComplete(r)) {
        ++it;
        continue;
      } else {
        for (const EdgeChannelRef& e : r.request.removed_edges) e->MarkRetired(false);
      }
      done.push_back(std::move(r));
      it = retired_.erase(it);
    }
    retire_pending_.store(static_cast<std::uint32_t>(retired_.size()), std::memory_order_release);
  }
  if (events_.on_drain_timeout) {
    for (const TopologyVersion v : timed_out) events_.on_drain_timeout(v);
  }
  bool progressed = !done.empty() || !timed_out.empty();
  for (const NodeRuntimeRef& n : to_close) progressed = TryCloseCancelled(*n) || progressed;
  for (Retired& r : done) {
    if (r.request.on_complete) r.request.on_complete(r.timed_out);
  }
  return progressed;
}

void Scheduler::Tick() {
  if (!alive_.load(std::memory_order_acquire)) return;
  RetryInjections();
  if (retire_pending_.load(std::memory_order_acquire) == 0) return;
  // Retiring nodes that are idle with ended input (their upstream finished
  // before publish) need a nudge.
  std::vector<NodeRuntimeRef> nudge;
  {
    std::lock_guard lock(retire_mutex_);
    for (const Retired& r : retired_) {
      for (const NodeRuntimeRef& n : r.request.removed_nodes) nudge.push_back(n);
    }
  }
  for (const NodeRuntimeRef& n : nudge) {
    const NodeState s = n->state();
    if (s == NodeState::kDraining) {
      RetryBlocked(*n);
      continue;
    }
    if (s != NodeState::kReady && s != NodeState::kRunning) continue;
    if (n->cancelled()) {
      TryCloseCancelled(*n);
      continue;
    }
    MarkReady(*n);
  }
  // A drain may have completed between checks; loop until stable.
  while (CompleteRetired()) {
  }
}

std::size_t Scheduler::pending_retirements() const {
  std::lock_guard lock(retire_mutex_);
  return retired_.size();
}

}  // namespace ge
