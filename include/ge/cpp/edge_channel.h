#ifndef GE_CPP_EDGE_CHANNEL_H_
#define GE_CPP_EDGE_CHANNEL_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <ge/cpp/capability.h>
#include <ge/cpp/graph_spec.h>
#include <ge/cpp/packet.h>
#include <ge/cpp/types.h>

namespace ge {

class NodeRuntime;

// 12 §2.4a
enum class EdgeState : std::uint8_t { kActive, kBackpressured, kDraining, kRetired };
[[nodiscard]] std::string_view ToString(EdgeState s) noexcept;

// 12 §2.3
struct EdgeConfig {
  std::uint32_t capacity = 64;
  DropPolicy policy = DropPolicy::kBlock;
  SyncPolicy sync_policy = SyncPolicy::kAny;
};

// 12 §6.2 per-edge counters. Relaxed atomics; readers snapshot.
struct EdgeMetrics {
  std::atomic<std::uint32_t> queue_depth{0};
  std::atomic<std::uint32_t> max_depth{0};
  std::atomic<std::uint64_t> pushed{0};
  std::atomic<std::uint64_t> popped{0};
  std::atomic<std::uint64_t> drop_count{0};
  std::atomic<std::uint64_t> would_block_count{0};
  std::atomic<std::uint64_t> cancelled_count{0};

  struct Snapshot {
    std::uint32_t queue_depth, max_depth;
    std::uint64_t pushed, popped, drop_count, would_block_count, cancelled_count;
  };
  [[nodiscard]] Snapshot Load() const noexcept;
};

enum class PushOutcome : std::uint8_t {
  kAccepted,
  kAcceptedDroppedOldest,  // drop_oldest: an older packet was evicted
  kDroppedNewest,          // drop_newest: this packet was discarded (counted)
  kWouldBlock,             // block: edge marked backpressured
  kCancelled,              // draining/retired edge
};

// Bounded FIFO with one producer and one consumer (12 §2.3). The engine
// guarantees that at most one thread produces (the upstream node's invoke)
// and at most one consumes (the downstream node's invoke) at any time;
// drop_oldest performs the pop on the producer side under a light lock that
// the consumer also takes only for Pop(), so both directions stay wait-free
// in the common case. The lock is the honest choice over a lock-free SPSC
// ring because drop_oldest needs producer-side eviction.
class EdgeChannel final {
 public:
  EdgeChannel(EdgeId id, std::string external_id, ConnectionContract contract, EdgeConfig config,
              PortRef from, PortRef to);

  [[nodiscard]] EdgeId id() const noexcept { return id_; }
  [[nodiscard]] const std::string& external_id() const noexcept { return external_id_; }
  [[nodiscard]] const ConnectionContract& contract() const noexcept { return contract_; }
  [[nodiscard]] const EdgeConfig& config() const noexcept { return config_; }
  [[nodiscard]] const PortRef& from() const noexcept { return from_; }
  [[nodiscard]] const PortRef& to() const noexcept { return to_; }
  [[nodiscard]] EdgeMetrics& metrics() noexcept { return metrics_; }
  [[nodiscard]] const EdgeMetrics& metrics() const noexcept { return metrics_; }

  [[nodiscard]] EdgeState state() const noexcept { return state_.load(std::memory_order_acquire); }
  // Producer/consumer NodeRuntime (set by RuntimeTopology::Build). Lets the
  // scheduler re-mark the producer after a Pop without a topology lookup.
  void Bind(std::weak_ptr<NodeRuntime> producer, std::weak_ptr<NodeRuntime> consumer) noexcept {
    producer_ = std::move(producer);
    consumer_ = std::move(consumer);
  }
  [[nodiscard]] std::shared_ptr<NodeRuntime> producer() const noexcept { return producer_.lock(); }
  [[nodiscard]] std::shared_ptr<NodeRuntime> consumer() const noexcept { return consumer_.lock(); }
  // Topology retire: active/backpressured -> draining (12 §2.4a).
  void MarkDraining() noexcept;
  // Drain complete or fast clear: -> retired. Fast clears the queue and
  // releases every PacketRef (12 §7.7).
  void MarkRetired(bool fast) noexcept;

  // 12 §6.2. EOS packets bypass the drop policy and always take a slot when
  // one is free; when full they report kWouldBlock so the caller re-tries
  // (12 §4.4 step 5, block semantics without sleeping).
  [[nodiscard]] PushOutcome Push(PacketRef packet);
  // Engine-only (12 §7.7 Drain): appends an EOS marker to a draining edge so
  // the consumer learns that no producer will follow. Accepted even while
  // draining; kWouldBlock if the queue is full (caller retries).
  [[nodiscard]] PushOutcome InjectEos(PacketRef eos);
  // Non-consuming reservation used by Scheduler before re-marking upstream.
  [[nodiscard]] std::optional<PacketRef> Pop();
  // Snapshot of the head (nullptr when empty). Returned by value: the ring
  // may be mutated by the producer (drop_oldest) or a fast retire.
  [[nodiscard]] PacketRef Peek() const;
  [[nodiscard]] std::uint32_t depth() const noexcept {
    return metrics_.queue_depth.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool empty() const noexcept { return depth() == 0; }
  [[nodiscard]] bool full() const noexcept { return depth() >= config_.capacity; }

  // Scheduler hook: after a Pop freed a slot, backpressured -> active.
  // Returns true if the transition happened (upstream should be re-marked).
  bool ClearBackpressure() noexcept;
  // Scheduler hook (12 §6.2): producer observed a full block-policy queue
  // before pushing. active -> backpressured; returns true only if the queue
  // is still full afterwards (otherwise the transition is undone).
  bool MarkBackpressured() noexcept;

 private:
  void PushLocked(PacketRef packet);
  [[nodiscard]] bool TailHasEos() const noexcept;  // mutex_ held
  void BumpDepth(std::uint32_t depth) noexcept;

  EdgeId id_;
  std::string external_id_;
  ConnectionContract contract_;
  EdgeConfig config_;
  PortRef from_;
  PortRef to_;
  std::atomic<EdgeState> state_{EdgeState::kActive};
  EdgeMetrics metrics_;
  std::weak_ptr<NodeRuntime> producer_;
  std::weak_ptr<NodeRuntime> consumer_;

  mutable std::mutex mutex_;
  std::vector<PacketRef> ring_;
  std::uint32_t head_ = 0;  // next pop
  std::uint32_t size_ = 0;
};

using EdgeChannelRef = std::shared_ptr<EdgeChannel>;

}  // namespace ge

#endif
