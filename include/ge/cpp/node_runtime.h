#ifndef GE_CPP_NODE_RUNTIME_H_
#define GE_CPP_NODE_RUNTIME_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ge/cpp/atomic_shared_ptr.h>
#include <ge/cpp/capability.h>
#include <ge/cpp/graph_spec.h>
#include <ge/cpp/input_binding.h>
#include <ge/cpp/json.h>
#include <ge/cpp/operator.h>
#include <ge/cpp/types.h>

namespace ge {

class RuntimeTopology;

// 12 §6.1
enum class NodeState : std::uint8_t {
  kCreated, kOpening, kReady, kRunning, kAsyncPending, kDraining, kClosing, kClosed, kFailed
};
[[nodiscard]] std::string_view ToString(NodeState s) noexcept;

// ---------------------------------------------------------------------------
// ParameterStore (12 §5): control plane writes serially, data plane reads
// lock-free through the atomic shared_ptr.
// ---------------------------------------------------------------------------

struct ParameterSnapshot {
  ParameterVersion version = 0;
  JsonValue values = JsonValue(JsonObject{});
  PacketSeq effective_after_seq = 0;
};

struct ParameterAuditRecord {
  ParameterVersion version;
  PacketSeq effective_after_seq;
  std::string values_json;
};

struct ParameterUpdateRequest {
  OperationId operation = 0;
  JsonValue values;  // keys to overwrite
};

class ParameterStore final {
 public:
  explicit ParameterStore(JsonValue initial, std::size_t history_capacity = 64);

  [[nodiscard]] std::shared_ptr<const ParameterSnapshot> Current() const noexcept {
    return current_.load();
  }
  // Control plane: validated request becomes pending (version+1). A second
  // request while one is pending is queued (PAR-5). |hot_updatable| lists
  // keys allowed to change; empty list == nothing hot-updatable.
  // Returns the version the update will carry once it takes effect.
  [[nodiscard]] Result<ParameterVersion> Submit(ParameterUpdateRequest request,
                                                const std::vector<std::string>& hot_updatable);
  // Data plane, before BeginInvoke: pending -> current; returns the new
  // version if a switch happened.
  std::optional<ParameterVersion> SwitchAtPacketBoundary(PacketSeq seq);
  [[nodiscard]] bool has_pending() const;
  [[nodiscard]] std::vector<ParameterAuditRecord> history() const;
  // Plugin rejected the update: drop pending, keep current (12 §5).
  void RejectPending();

 private:
  void PromoteQueued();

  AtomicSharedPtr<const ParameterSnapshot> current_;
  mutable std::mutex mutex_;
  std::shared_ptr<ParameterSnapshot> pending_;
  std::deque<ParameterUpdateRequest> queued_;
  std::deque<ParameterAuditRecord> history_;
  std::size_t history_capacity_;
  std::vector<std::string> last_hot_updatable_;
};

// ---------------------------------------------------------------------------
// NodeMetrics (12 §12.1 subset needed by P2/P5)
// ---------------------------------------------------------------------------

// 12 §12.1 fixed-bucket, lock-free latency histogram. Bucket i holds
// samples < 2^(i+10) ns (1µs .. ~33s), the last bucket everything above.
class LatencyHistogram final {
 public:
  static constexpr std::size_t kBuckets = 16;
  void Record(std::uint64_t ns) noexcept;
  [[nodiscard]] std::uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t sum_ns() const noexcept { return sum_ns_.load(std::memory_order_relaxed); }
  // Upper bound of the bucket holding the requested quantile (0 if empty).
  [[nodiscard]] std::uint64_t Quantile(double q) const noexcept;
  [[nodiscard]] std::uint64_t P50() const noexcept { return Quantile(0.5); }
  [[nodiscard]] std::uint64_t P99() const noexcept { return Quantile(0.99); }
  [[nodiscard]] static std::uint64_t BucketUpperNs(std::size_t bucket) noexcept;

 private:
  std::array<std::atomic<std::uint64_t>, kBuckets> buckets_{};
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::uint64_t> sum_ns_{0};
};

struct NodeMetrics {
  std::atomic<std::uint64_t> invocations{0};
  std::atomic<std::uint64_t> packets_in{0};
  std::atomic<std::uint64_t> packets_out{0};
  std::atomic<std::uint64_t> errors{0};
  std::atomic<std::uint64_t> would_block{0};
  std::atomic<std::uint64_t> process_ns_total{0};
  // Async (12 §12.1): submit -> completion, batch sizes, gaps, orphans.
  std::atomic<std::uint64_t> submitted{0};
  std::atomic<std::uint64_t> completed{0};
  LatencyHistogram async_wait;
  std::atomic<std::uint64_t> batch_count{0};
  std::atomic<std::uint64_t> batch_size_sum{0};
  std::atomic<std::uint64_t> reorder_gaps{0};
  std::atomic<std::uint64_t> late_completions{0};
  std::atomic<std::uint64_t> orphan_completions{0};
};

// ---------------------------------------------------------------------------
// NodeRuntime (12 §2.4): identity, operator instance, state, parameters,
// in-flight accounting and the ready-state machine of 12 §6.1. Topology
// (routes, inputs) is *not* owned here.
// ---------------------------------------------------------------------------

class NodeRuntime final : public std::enable_shared_from_this<NodeRuntime> {
 public:
  NodeRuntime(NodeId id, std::string external_id, OperatorKey key,
              const CapabilityDescriptor& capability, std::unique_ptr<Operator> op,
              JsonValue options, std::uint32_t parallelism);

  [[nodiscard]] NodeId id() const noexcept { return id_; }
  [[nodiscard]] const std::string& external_id() const noexcept { return external_id_; }
  [[nodiscard]] const OperatorKey& operator_key() const noexcept { return key_; }
  [[nodiscard]] const CapabilityDescriptor& capability() const noexcept { return capability_; }
  [[nodiscard]] Operator& op() noexcept { return *op_; }
  [[nodiscard]] ParameterStore& parameters() noexcept { return parameters_; }
  [[nodiscard]] const JsonValue& options() const noexcept { return options_; }
  [[nodiscard]] std::uint32_t max_parallelism() const noexcept { return max_parallelism_; }
  [[nodiscard]] NodeMetrics& metrics() noexcept { return metrics_; }
  [[nodiscard]] bool is_source() const noexcept { return capability_.inputs.empty(); }
  [[nodiscard]] bool is_sink() const noexcept { return capability_.outputs.empty(); }

  [[nodiscard]] NodeState state() const noexcept { return state_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint32_t in_flight() const noexcept {
    return in_flight_.load(std::memory_order_acquire);
  }
  [[nodiscard]] PacketSeq last_seq() const noexcept { return last_seq_.load(std::memory_order_relaxed); }
  [[nodiscard]] const Status& failure() const noexcept { return failure_; }

  // Lifecycle (engine only).
  [[nodiscard]] Status Open(const OpenRequest& request);
  [[nodiscard]] Status Close(const CloseRequest& request);
  void Fail(Status status);

  // 12 §6.1: TrySchedule reserves one slot (single node: Idle->Ready CAS;
  // N>1: available_slots--). Returns false when no slot / not schedulable.
  [[nodiscard]] bool TrySchedule() noexcept;
  // Releases a slot without invoking (input turned out not ready).
  void CancelSchedule() noexcept;
  // Ready -> Running; in_flight++.
  void BeginInvoke() noexcept;
  // in_flight--; Running -> Ready (or keeps draining/closing states).
  void EndInvoke() noexcept;
  void NoteSeq(PacketSeq seq) noexcept { last_seq_.store(seq, std::memory_order_relaxed); }

  // EOS lifecycle (12 §4.4 step 3/4).
  [[nodiscard]] bool TryEnterDraining() noexcept;  // ready/running & in_flight==0 -> draining
  void EnterClosing() noexcept;

  // Async accounting (12 §4.3 step 6, §4.4 step 7): requests submitted to
  // the backend and not yet delivered/dropped. Draining waits for zero.
  // Async operators run single-slot (submission order == seq order).
  [[nodiscard]] bool is_async() const noexcept { return capability_.execution.async; }
  void BeginAsync() noexcept;
  // Returns true when the node left async_pending (a slot below the cap
  // freed up or the count reached zero) and should be re-marked.
  bool EndAsync() noexcept;
  [[nodiscard]] std::uint32_t async_pending() const noexcept {
    return async_pending_.load(std::memory_order_acquire);
  }
  // Node option "async_max_in_flight" (default 16): at the cap the node
  // stops being scheduled until a completion arrives.
  [[nodiscard]] std::uint32_t async_max_in_flight() const noexcept { return async_max_in_flight_; }
  // ready -> async_pending (TrySchedule fails until EndAsync flips back).
  // Callers re-check async_pending() afterwards and TryLeaveAsyncPending()
  // if a completion raced in (Dekker with EndAsync).
  void EnterAsyncPending() noexcept;
  [[nodiscard]] bool TryLeaveAsyncPending() noexcept;

  // Fast-retire marker: subsequent emits are rejected (12 §7.7).
  void MarkCancelled() noexcept { cancelled_.store(true, std::memory_order_release); }
  [[nodiscard]] bool cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }
  // Drain-retire marker (12 §7.7): a removed source stops pulling and
  // finishes (flush + EOS) on its next turn.
  void MarkRetiring() noexcept { retiring_.store(true, std::memory_order_release); }
  [[nodiscard]] bool retiring() const noexcept { return retiring_.load(std::memory_order_acquire); }

  // MUT-3 bounded local pause: while held, TrySchedule() fails so the
  // coordinator can wait for in_flight == 0 before retiring an output edge
  // the running invocation may still emit to. Release() re-enables; the
  // caller re-marks the node ready.
  void Hold() noexcept { held_.store(true, std::memory_order_seq_cst); }
  void Release() noexcept { held_.store(false, std::memory_order_seq_cst); }
  [[nodiscard]] bool held() const noexcept { return held_.load(std::memory_order_seq_cst); }
  // Scheduler-side "an invoke task is on the stack" counter (covers the
  // whole Invoke body, including flush/EOS after EndInvoke).
  void EnterBusy() noexcept { busy_.fetch_add(1, std::memory_order_seq_cst); }
  void ExitBusy() noexcept { busy_.fetch_sub(1, std::memory_order_seq_cst); }
  [[nodiscard]] bool busy() const noexcept { return busy_.load(std::memory_order_seq_cst) != 0; }
  // True when no invoke task is running or scheduled (every slot free).
  [[nodiscard]] bool quiescent() const noexcept {
    return busy_.load(std::memory_order_seq_cst) == 0 &&
           available_slots_.load(std::memory_order_seq_cst) == max_parallelism_;
  }

  // Routing snapshot (12 §4.1 step 1 / §7.7): the topology whose RouteTable
  // this node emits through. Kept nodes are moved to Vn+1 at publish;
  // removed nodes keep Vn until closed.
  void SetTopology(std::shared_ptr<const RuntimeTopology> topology) noexcept {
    topology_.store(std::move(topology));
  }
  [[nodiscard]] std::shared_ptr<const RuntimeTopology> topology() const noexcept {
    return topology_.load();
  }

 private:
  NodeId id_;
  std::string external_id_;
  OperatorKey key_;
  CapabilityDescriptor capability_;
  std::unique_ptr<Operator> op_;
  JsonValue options_;
  std::uint32_t max_parallelism_;
  std::uint32_t async_max_in_flight_ = 16;
  ParameterStore parameters_;
  std::atomic<NodeState> state_{NodeState::kCreated};
  std::atomic<std::uint32_t> in_flight_{0};
  std::atomic<std::uint32_t> async_pending_{0};
  std::atomic<std::uint32_t> busy_{0};
  std::atomic<std::uint32_t> available_slots_;
  std::atomic<PacketSeq> last_seq_{0};
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> retiring_{false};
  std::atomic<bool> held_{false};
  AtomicSharedPtr<const RuntimeTopology> topology_;
  NodeMetrics metrics_;
  Status failure_;
};

using NodeRuntimeRef = std::shared_ptr<NodeRuntime>;

}  // namespace ge

#endif
