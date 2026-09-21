#ifndef GE_CPP_ASYNC_RUNTIME_H_
#define GE_CPP_ASYNC_RUNTIME_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <ge/cpp/json.h>
#include <ge/cpp/node_runtime.h>
#include <ge/cpp/operator.h>
#include <ge/cpp/runtime_topology.h>
#include <ge/cpp/types.h>

// C handle body behind ge_completion_sink_handle (13 §4.6). Lives inside the
// engine's CompletionSink; the magic lets completion_push reject stale or
// foreign handles without dereferencing them.
struct ge_completion_sink_t {
  std::uint64_t magic;
  ge::CompletionSink* sink;
};

namespace ge {

class AsyncRuntime;

// ---------------------------------------------------------------------------
// Options (12 §4.3, ASY-6/7). Batching is decided per request from the
// global switch, the session switch and the node's "batch" options:
//   {"batch": {"enabled": bool, "max_batch": N, "timeout_ms": M}}
// ---------------------------------------------------------------------------

struct AsyncOptions {
  std::uint32_t completion_queue_capacity = 4096;
  bool batching = true;
  std::uint32_t max_batch = 8;
  std::chrono::milliseconds batch_timeout{10};
  // Requests without a capability max_inference_ms (cannot happen for
  // validated async operators) fall back to this deadline.
  std::chrono::milliseconds default_inference_timeout{1000};
  // true: a dedicated consumer thread; false: the host calls Pump().
  bool worker_thread = true;
};

struct NodeBatchConfig {
  bool enabled = true;
  std::optional<std::uint32_t> max_batch;
  std::optional<std::chrono::milliseconds> timeout;
  [[nodiscard]] static NodeBatchConfig FromOptions(const JsonValue& node_options);
};

// Scheduler-side hooks of one Session, invoked on the runtime's consumer
// thread (worker or Pump caller).
struct AsyncSessionHooks {
  // Packets were pushed to downstream edges: wake the consumers.
  std::function<void(const EmitReport& report)> after_emit;
  // async_pending of |node| dropped to zero (12 §4.3 step 6).
  std::function<void(NodeRuntime& node)> on_async_idle;
  // A completion carried an error or submit failed: the runtime already
  // marked the node failed.
  std::function<void(NodeRuntime& node, const Status& status)> on_node_failed;
};

// ---------------------------------------------------------------------------
// CompletionQueue (12 §4.3): bounded multi-producer queue drained by the
// AsyncRuntime consumer. Full => RESOURCE_EXHAUSTED and dropped_on_full++.
// ---------------------------------------------------------------------------

class CompletionQueue final {
 public:
  explicit CompletionQueue(std::uint32_t capacity);
  [[nodiscard]] Status Push(CompletionEvent event);
  // Internal producers (submit failures) bypass the capacity check.
  void ForcePush(CompletionEvent event);
  std::vector<CompletionEvent> DrainAll();
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::uint64_t dropped_on_full() const noexcept {
    return dropped_on_full_.load(std::memory_order_relaxed);
  }
  // Wakes the consumer; set by the runtime.
  void set_notify(std::function<void()> notify) { notify_ = std::move(notify); }

 private:
  const std::uint32_t capacity_;
  mutable std::mutex mutex_;
  std::deque<CompletionEvent> events_;
  std::atomic<std::uint64_t> dropped_on_full_{0};
  std::function<void()> notify_;
};

// ---------------------------------------------------------------------------
// SessionCompletionSink: the per-Session sink handed to operators. Alive
// from registration until AsyncRuntime::DetachSession; pushes after that
// return CANCELLED.
// ---------------------------------------------------------------------------

class SessionCompletionSink final : public CompletionSink {
 public:
  SessionCompletionSink(AsyncRuntime& runtime, SessionId session);
  ~SessionCompletionSink() override;
  Status Push(CompletionEvent event) override;
  [[nodiscard]] ge_completion_sink_t* c_handle() noexcept { return &body_; }
  [[nodiscard]] SessionId session() const noexcept { return session_; }
  void Detach() noexcept { alive_.store(false, std::memory_order_release); }
  [[nodiscard]] static SessionCompletionSink* FromHandle(ge_completion_sink_t* handle) noexcept;

 private:
  AsyncRuntime& runtime_;
  SessionId session_;
  std::atomic<bool> alive_{true};
  ge_completion_sink_t body_;
};

// ---------------------------------------------------------------------------
// AsyncRuntime (12 §4.2–4.3, §6.4, 13 §7.2): the only component that turns
// completion events into data-plane output. Owns the CompletionQueue, the
// in-flight request table, per-(node, topology) ReorderBuffers and the
// Batcher. Shared by every Session of an Engine so batches may span
// sessions (ASY-5).
// ---------------------------------------------------------------------------

class AsyncRuntime final {
 public:
  explicit AsyncRuntime(AsyncOptions options = {});
  ~AsyncRuntime();
  AsyncRuntime(const AsyncRuntime&) = delete;
  AsyncRuntime& operator=(const AsyncRuntime&) = delete;

  // Session registration. |batching| is the session-level switch (ASY-7).
  std::shared_ptr<SessionCompletionSink> AttachSession(SessionId session, AsyncSessionHooks hooks,
                                                       bool batching);
  // Orphans every in-flight request of the session and invalidates its
  // sink. Called after the session's nodes are closed.
  void DetachSession(SessionId session);

  struct SubmitArgs {
    NodeRuntimeRef node;
    std::shared_ptr<const RuntimeTopology> topology;
    std::shared_ptr<const ParameterSnapshot> parameters;
    std::vector<std::string> input_ports;
    std::vector<PacketRef> inputs;
    PacketSeq packet_seq = 0;
    std::int32_t device_id = -1;
  };
  // Scheduler entry (12 §4.2). Registers the request, then either submits
  // right away (batching off / max_batch 1) or parks it in the Batcher.
  // The node's async_pending was already incremented by the caller; the
  // runtime decrements it when the result is delivered, dropped or the
  // submit fails. Returns the request id.
  RequestId Submit(SubmitArgs args);

  // One consumer iteration: completions, due batches, reorder deadlines.
  // Safe from any thread; used by hosts without a worker thread and by
  // waiters that must make progress on the calling thread.
  bool Pump();
  [[nodiscard]] CompletionQueue& queue() noexcept { return queue_; }
  [[nodiscard]] const AsyncOptions& options() const noexcept { return options_; }

  // Observability (12 §12.1, 15 P5 task 7).
  [[nodiscard]] std::uint64_t in_flight() const noexcept {
    return in_flight_count_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t orphan_completion_total() const noexcept {
    return orphan_total_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t timeout_gap_total() const noexcept {
    return gap_total_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t late_completion_total() const noexcept {
    return late_total_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t batches_flushed() const noexcept {
    return batches_flushed_.load(std::memory_order_relaxed);
  }
  // Results whose downstream edge was full and that wait for a slot.
  [[nodiscard]] std::size_t blocked_outputs() const;
  [[nodiscard]] JsonValue Stats() const;

 private:
  friend class SessionCompletionSink;

  struct BatchKey {
    std::string operator_key;
    std::string model;
    std::int32_t device_id = -1;
    std::string parameters;  // serialised snapshot content (ASY-8)
    friend auto operator<=>(const BatchKey&, const BatchKey&) = default;
  };

  struct Entry {
    RequestId id = 0;
    SessionId session = 0;
    TopologyVersion topology = 0;
    ParameterVersion parameter = 0;
    NodeRuntimeRef node;
    std::shared_ptr<const RuntimeTopology> topology_ref;
    PacketSeq seq = 0;
    std::chrono::steady_clock::time_point enqueued;
    std::optional<std::chrono::steady_clock::time_point> submitted;  // set at flush
    std::chrono::steady_clock::time_point deadline;                  // valid once submitted
    // Result parked in the reorder buffer until the head of line clears.
    std::optional<CompletionEvent> result;
    bool done = false;
  };

  struct Pending {  // parked in the Batcher
    RequestId id = 0;
    std::shared_ptr<const ParameterSnapshot> parameters;
    std::vector<std::string> input_ports;
    std::vector<PacketRef> inputs;
    std::chrono::steady_clock::time_point due;
    std::uint32_t max_batch = 1;
  };

  // Hooks capture the Session; the consumer thread invokes them with
  // state_mutex_ released, so a concurrent ~Session -> DetachSession must
  // wait for a hook already in flight before the Session memory goes away
  // (ASan on Linux CI: OnAsyncNodeFailed -> Session::OnNodeFailed after
  // the session was freed). |guard| is shared by the map entry and every
  // snapshot: hooks run under a shared lock, DetachSession flips |alive|
  // under an exclusive one and thereby waits for in-flight hooks. Hooks
  // never call back into DetachSession, so there is no self-deadlock.
  struct HookGuard {
    std::shared_mutex mutex;
    bool alive = true;
  };
  struct SessionEntry {
    std::shared_ptr<SessionCompletionSink> sink;
    AsyncSessionHooks hooks;
    bool batching = true;
    std::shared_ptr<HookGuard> guard;

    template <typename Hook, typename... Args>
    void Call(const Hook& hook, Args&&... args) const {
      if (!hook || !guard) return;
      std::shared_lock lock(guard->mutex);
      if (!guard->alive) return;
      hook(std::forward<Args>(args)...);
    }
    void AfterEmit(const EmitReport& report) const { Call(hooks.after_emit, report); }
    void OnAsyncIdle(NodeRuntime& node) const { Call(hooks.on_async_idle, node); }
    void OnNodeFailed(NodeRuntime& node, const Status& status) const { Call(hooks.on_node_failed, node, status); }
  };

  struct ReorderKey {
    NodeRuntime* node = nullptr;
    TopologyVersion topology = 0;
    friend auto operator<=>(const ReorderKey&, const ReorderKey&) = default;
  };

  // Submission-ordered queue of request ids for one (node, topology).
  struct ReorderBuffer {
    std::deque<RequestId> order;
  };

  // 12 §6.2 block semantics for async output: a delivered result whose
  // downstream edge was full is parked here (per blocked edge) and pushed
  // again on every consumer iteration. While a node has parked output its
  // reorder buffer does not advance, so edge order equals seq order.
  struct BlockedOutput {
    NodeRuntimeRef node;
    std::shared_ptr<const RuntimeTopology> topology;
    SessionId session = 0;
    std::vector<std::pair<EdgeChannel*, PacketRef>> pushes;
  };

  void Worker();
  void Wake();
  bool DrainCompletions();
  bool FlushDueBatches(std::chrono::steady_clock::time_point now);
  void FlushBatch(const BatchKey& key, std::vector<Pending> members);
  bool ExpireDeadlines(std::chrono::steady_clock::time_point now);
  bool RetryBlockedOutputs();
  bool DeliverReady();
  void OnCompletion(CompletionEvent event);
  // Delivers or drops finished heads of every reorder buffer; state_mutex_
  // must not be held. Returns true if anything was delivered.
  void DeliverEntry(Entry& entry, const SessionEntry* session);
  void EmitGap(Entry& entry, const SessionEntry* session);
  void FinishEntry(Entry& entry, const SessionEntry* session);
  void FailNode(Entry& entry, const SessionEntry* session, const Status& status);
  std::optional<std::chrono::steady_clock::time_point> NextWake() const;  // state_mutex_ held
  static std::int32_t DeviceOf(const RuntimeTopology& topology, const NodeRuntime& node);

  AsyncOptions options_;
  CompletionQueue queue_;
  std::atomic<RequestId> next_request_{1};
  std::atomic<std::uint64_t> next_batch_{1};
  std::atomic<std::uint64_t> in_flight_count_{0};
  std::atomic<std::uint64_t> orphan_total_{0};
  std::atomic<std::uint64_t> gap_total_{0};
  std::atomic<std::uint64_t> late_total_{0};
  std::atomic<std::uint64_t> batches_flushed_{0};

  mutable std::mutex state_mutex_;
  std::map<SessionId, SessionEntry> sessions_;
  std::map<RequestId, Entry> in_flight_;
  std::map<ReorderKey, ReorderBuffer> reorder_;
  std::map<ReorderKey, BlockedOutput> blocked_;
  std::map<BatchKey, std::vector<Pending>> batches_;
  // Serialises plugin submit calls so batches never interleave (13 §4.6).
  std::mutex submit_mutex_;
  std::mutex pump_mutex_;

  std::mutex wake_mutex_;
  std::condition_variable wake_cv_;
  bool wake_pending_ = false;  // guarded by wake_mutex_
  bool stop_ = false;          // guarded by wake_mutex_
  std::thread worker_;
};

}  // namespace ge

#endif
