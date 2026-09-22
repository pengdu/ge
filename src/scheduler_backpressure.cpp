// Scheduler: block-policy backpressure (docs/16 §4, 12 §6.2).
//   OutputsBlocked (do not invoke while a block edge is full), the parked
//   packet store (Park / HasParked / RetryParked / DropParked) and
//   RetryBlocked (pending EOS behind parked data).
#include <ge/cpp/scheduler.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>

#include "scheduler_internal.h"

namespace ge {

bool Scheduler::OutputsBlocked(NodeRuntime& node, const RuntimeTopology& topo) {
  bool blocked = false;
  for (const std::string& port : topo.OutputPorts(node.id())) {
    const auto* routes = topo.RoutesFor(node.id(), port);
    if (routes == nullptr) continue;
    for (const RouteEntry& e : *routes) {
      if (e.edge->config().policy != DropPolicy::kBlock) continue;
      if (e.edge->full() && e.edge->MarkBackpressured()) blocked = true;
    }
  }
  return blocked;
}

// ---------------------------------------------------------------------------
// 12 §6.2 parked output
// ---------------------------------------------------------------------------

bool Scheduler::HasParked(const NodeRuntime& node) const {
  if (parked_nodes_.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard lock(park_mutex_);
  return parked_.contains(const_cast<NodeRuntime*>(&node));
}

bool Scheduler::HasParked(const NodeRuntime& node, const EdgeChannel& edge) const {
  if (parked_nodes_.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard lock(park_mutex_);
  const auto it = parked_.find(const_cast<NodeRuntime*>(&node));
  if (it == parked_.end()) return false;
  return std::any_of(it->second.begin(), it->second.end(),
                     [&](const auto& p) { return p.first.get() == &edge; });
}

void Scheduler::Park(NodeRuntime& node, EdgeChannel& edge, PacketRef packet) {
  EdgeChannelRef ref;
  if (const auto topo = node.topology()) ref = topo->SharedEdgeFor(&edge);
  if (!ref) return;  // edge no longer part of the node's topology: dropped
  std::lock_guard lock(park_mutex_);
  auto& list = parked_[&node];
  if (list.empty()) parked_nodes_.fetch_add(1, std::memory_order_acq_rel);
  list.emplace_back(std::move(ref), std::move(packet));
}

void Scheduler::DropParked(NodeRuntime& node) {
  std::lock_guard lock(park_mutex_);
  if (parked_.erase(&node) != 0) parked_nodes_.fetch_sub(1, std::memory_order_acq_rel);
}

bool Scheduler::RetryParked(NodeRuntime& node) {
  // One retry pass at a time per scheduler: two concurrent passes would
  // both push the same snapshot.
  std::lock_guard retry_lock(park_retry_mutex_);
  std::vector<std::pair<EdgeChannelRef, PacketRef>> work;
  {
    std::lock_guard lock(park_mutex_);
    const auto it = parked_.find(&node);
    if (it == parked_.end()) return true;
    work = it->second;
  }
  if (node.cancelled()) {
    DropParked(node);
    return true;
  }
  EmitReport report;
  std::vector<std::pair<EdgeChannelRef, PacketRef>> still;
  // Edges that already refused a packet in this pass: everything queued
  // behind them stays parked (per-edge FIFO), without rescanning |still|.
  std::set<const EdgeChannel*> blocked;
  for (auto& [edge, packet] : work) {
    if (blocked.contains(edge.get())) {
      still.emplace_back(edge, packet);
      continue;
    }
    const PushOutcome o = PacketRouter::PushOne(*edge, packet, node, &report);
    if (o == PushOutcome::kWouldBlock) {
      blocked.insert(edge.get());
      still.emplace_back(edge, packet);
    }
  }
  bool empty = false;
  {
    std::lock_guard lock(park_mutex_);
    const auto it = parked_.find(&node);
    if (it != parked_.end()) {
      // Emits that raced in meanwhile were appended after |work|.
      const std::size_t appended = it->second.size() - work.size();
      still.insert(still.end(), it->second.end() - static_cast<std::ptrdiff_t>(appended), it->second.end());
      if (still.empty()) {
        parked_.erase(it);
        parked_nodes_.fetch_sub(1, std::memory_order_acq_rel);
        empty = true;
      } else {
        it->second = std::move(still);
      }
    } else {
      empty = true;
    }
  }
  for (NodeRuntime* t : report.targets) MarkReady(*t);
  return empty;
}

// Sends pending EOS for |node|; when every output delivered, closes it.
void Scheduler::RetryBlocked(NodeRuntime& node) {
  // EOS may only follow the data parked ahead of it (12 §6.2 FIFO).
  if (HasParked(node) && !RetryParked(node)) return;
  std::vector<NodeRuntime*> wake;
  bool done = false;
  {
    std::lock_guard lock(retry_mutex_);
    auto it = eos_retry_.find(&node);
    if (it == eos_retry_.end()) return;
    const std::shared_ptr<const RuntimeTopology> topo = node.topology();
    const auto params = node.parameters().Current();
    for (auto pit = it->second.begin(); pit != it->second.end();) {
      EmitReport report;
      Status s = topo ? PacketRouter::EmitEos(*topo, node, *pit, params->version, &report)
                      : Status::Ok();
      wake.insert(wake.end(), report.targets.begin(), report.targets.end());
      if (s.code() == GE_STATUS_WOULD_BLOCK) {
        ++pit;
        continue;
      }
      pit = it->second.erase(pit);
    }
    if (it->second.empty()) {
      eos_retry_.erase(it);
      done = true;
    }
  }
  for (NodeRuntime* t : wake) MarkReady(*t);
  if (!done) return;
  if (node.state() != NodeState::kFailed) CloseNode(node, node.cancelled());
  OnNodeDone(node);
}
}  // namespace ge
