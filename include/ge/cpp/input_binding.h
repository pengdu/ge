#ifndef GE_CPP_INPUT_BINDING_H_
#define GE_CPP_INPUT_BINDING_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <ge/cpp/edge_channel.h>
#include <ge/cpp/packet.h>

namespace ge {

struct InputPortBinding {
  std::string port;
  EdgeChannelRef edge;  // exactly one edge per input port
  bool required = true;
  // Retired edges that fed this port in earlier topology versions and still
  // marker before |edge| is touched, so old packets keep the old path.
  std::vector<EdgeChannelRef> predecessors;
};

struct InputBatch {
  std::vector<std::string> ports;
  std::vector<PacketRef> packets;  // parallel to |ports|
  // Packets that skipped synchronisation and go straight to on_event
  std::vector<PacketRef> events;
};

struct AlignedOptions {
  std::int64_t window_ns = 40'000'000;  // ±40ms
  std::optional<std::string> reference_port;  // nullopt: earliest ready required port
};

// Mutation that changes the node's input edges calls Rebind() at publish.
class InputBinding final {
 public:
  InputBinding(std::vector<InputPortBinding> ports, SyncPolicy policy,
               AlignedOptions aligned = {});

  [[nodiscard]] SyncPolicy policy() const;
  // Snapshot (the live table may be rebound concurrently).
  [[nodiscard]] std::vector<InputPortBinding> ports() const;
  [[nodiscard]] std::optional<InputPortBinding> Find(std::string_view port) const;

  // previous edge (and its unfinished predecessors) become predecessors of
  // the new one. EOS state of unchanged ports is kept.
  void Rebind(std::vector<InputPortBinding> ports, SyncPolicy policy, AlignedOptions aligned);
  // Drops a predecessor that was fast-retired (queue cleared).
  void ForgetPredecessor(const EdgeChannel& edge);
  // Every edge currently feeding this binding (ports + predecessors).
  [[nodiscard]] std::vector<EdgeChannelRef> Edges() const;

  // Returns a batch when the sync condition holds. Consumes packets from
  // the edges. Returns nullopt when nothing can be delivered yet.
  // Acquisition is serialized: a node with parallelism > 1 runs Process
  // single consumer of its input edges.
  [[nodiscard]] std::optional<InputBatch> TryAcquire();

  [[nodiscard]] bool InputEnded() const noexcept;
  [[nodiscard]] std::set<std::string> eos_ports() const;  // observation (tests/snapshot)
  [[nodiscard]] std::uint64_t late_dropped() const noexcept { return late_dropped_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t stale_dropped() const noexcept { return stale_dropped_.load(std::memory_order_relaxed); }

  // Cheap check used by Scheduler before enqueueing the node.
  [[nodiscard]] bool MayBeReady() const noexcept;

 private:
  // Pops leading EOS/event packets of |b| into state; returns the first data
  // packet still queued (not popped) or nullptr. Drains predecessors first.
  PacketRef SkimHead(InputPortBinding& b, InputBatch* out);
  // Pops the head of the edge SkimHead last looked at; nullptr if the edge
  // was retired/evicted concurrently.
  static PacketRef Take(InputPortBinding& b);
  // The edge SkimHead/Take currently operate on for |b|.
  [[nodiscard]] static EdgeChannel* Active(const InputPortBinding& b) noexcept;
  std::optional<InputBatch> AcquireAny();
  std::optional<InputBatch> AcquireLatest();
  std::optional<InputBatch> AcquireAligned();
  [[nodiscard]] bool IsEos(std::string_view port) const noexcept { return eos_ports_.contains(std::string(port)); }
  [[nodiscard]] bool InputEndedLocked() const noexcept;

  std::vector<InputPortBinding> ports_;
  SyncPolicy policy_;
  AlignedOptions aligned_;
  mutable std::mutex mutex_;  // guards ports_/eos_ports_ and edge Peek/Pop sequences
  std::set<std::string> eos_ports_;
  std::atomic<std::uint64_t> late_dropped_{0};   // aligned: outside window
  std::atomic<std::uint64_t> stale_dropped_{0};  // latest: superseded packets
};

}  // namespace ge

#endif
