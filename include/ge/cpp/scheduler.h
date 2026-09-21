#ifndef GE_CPP_SCHEDULER_H_
#define GE_CPP_SCHEDULER_H_

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
#include <set>
#include <thread>
#include <vector>

#include <ge/cpp/atomic_shared_ptr.h>
#include <ge/cpp/runtime_topology.h>
#include <ge/cpp/types.h>

namespace ge {

// CPU executor (12 §6.1 "CPU 使用 MPSC work queue"). GPU submit queues come
// with P5.
class ExecutorPool final {
 public:
  using Task = std::function<void()>;
  explicit ExecutorPool(std::uint32_t cpu_threads);
  ~ExecutorPool();

  void SubmitCpu(Task task);
  // Runs queued tasks on the calling thread (0 worker threads => inline
  // executor for deterministic tests). Returns true if anything ran.
  bool RunPending();
  // Runs exactly one queued task on the calling thread. Returns false if idle.
  bool RunOne();
  void Stop();  // running tasks finish, queued tasks dropped
  [[nodiscard]] std::uint32_t cpu_threads() const noexcept {
    return static_cast<std::uint32_t>(workers_.size());
  }

 private:
  void Worker();
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Task> queue_;
  std::vector<std::thread> workers_;
  bool stopped_ = false;
};

// P5 (12 §4.2): an async node's inputs are handed to the AsyncRuntime
// instead of Process. Set by the Session; null means async operators fail
// at their first invocation.
struct AsyncDispatch {
  struct Request {
    NodeRuntimeRef node;
    std::shared_ptr<const RuntimeTopology> topology;
    std::shared_ptr<const ParameterSnapshot> parameters;
    std::vector<std::string> input_ports;
    std::vector<PacketRef> inputs;
    PacketSeq packet_seq = 0;
  };
  std::function<void(Request request)> submit;
};

struct SchedulerEvents {
  // Called on the executor thread when a node fails; the scheduler has
  // already marked the node failed.
  std::function<void(NodeRuntime&, const Status&)> on_node_failed;
  // Called once every attached node is closed (12 §4.4 step 6).
  std::function<void()> on_all_closed;
  // 12 §7.7: a drain exceeded its deadline and was upgraded to fast.
  std::function<void(TopologyVersion retired_version)> on_drain_timeout;
  // P6: a builtin operator published an event (ProcessRequest::events).
  std::function<void(NodeRuntime&, std::string type, Severity, JsonValue detail)> on_operator_event;
};

// 12 §2.5 RetiredTopology + §7.7 progress. |removed_*| are filled by
// Publish() (nodes/edges of Vn absent from Vn+1).
struct RetireRequest {
  RemovePolicy policy = RemovePolicy::kDrain;
  std::chrono::milliseconds drain_timeout{2000};
  std::shared_ptr<const RuntimeTopology> topology;  // Vn (strong ref for in-flight work)
  std::vector<NodeRuntimeRef> removed_nodes;
  std::vector<EdgeChannelRef> removed_edges;
  // Invoked once (from a scheduler/coordinator thread) when every removed
  // node is closed and every removed edge is retired. |timed_out| means the
  // drain was upgraded to fast.
  std::function<void(bool timed_out)> on_complete;
};

// Data-driven scheduler for one Session (12 §4.1, §6.1, §6.2, §4.4, §7.7;
// state machine and file layout in docs/16-调度器状态机.md).
// Nodes route through the topology snapshot they are bound to
// (NodeRuntime::topology()); Publish() rebinds kept nodes to Vn+1 and
// retires Vn's removed nodes/edges by Drain or Fast.
class Scheduler final {
 public:
  Scheduler(std::shared_ptr<RuntimeTopology> topology, ExecutorPool& executor,
            SchedulerEvents events = {});
  ~Scheduler();

  // Must be set before Start(); async nodes route their batches here.
  void SetAsyncDispatch(AsyncDispatch dispatch) { async_ = std::move(dispatch); }
  // AsyncRuntime callbacks (any thread): downstream wake-ups after a
  // completion was routed, and node re-scheduling once its in-flight count
  // dropped below the cap (12 §4.3 step 6).
  void OnAsyncEmit(const EmitReport& report) { AfterEmit(report); }
  void OnAsyncIdle(NodeRuntime& node);
  void OnAsyncNodeFailed(NodeRuntime& node, const Status& status);

  [[nodiscard]] std::shared_ptr<RuntimeTopology> topology() const noexcept {
    return topology_.load();
  }

  // Opens every node of the initial topology (topological order); on
  // failure closes opened nodes. Nodes already open (P3 warm-up) are kept.
  [[nodiscard]] Status OpenAll() { return OpenNodes(*topology()); }
  // 12 §7.2 A7 warm-up: opens the still-created nodes of |candidate| in
  // topological order; on failure closes the ones this call opened and
  // leaves the running graph untouched.
  [[nodiscard]] Status OpenNodes(const RuntimeTopology& candidate);
  // Prepare failed after warm-up: closes the nodes created by |candidate|.
  void AbandonCandidate(const RuntimeTopology& candidate);
  // Marks sources ready and starts the data-driven loop.
  void Start();
  // Session pause/resume (12 §2.4a): sources stop being re-marked.
  void Pause();
  void Resume();
  // Stop: fast=false injects EOS at sources; fast=true cancels nodes,
  // clears edges and closes idle nodes immediately (busy nodes close when
  // their running call returns). Neither blocks; use WaitClosed().
  void Stop(bool fast);
  // Blocks until every attached node reached closed/failed (or timeout).
  [[nodiscard]] bool WaitClosed(std::uint64_t timeout_ms);
  [[nodiscard]] bool all_closed() const noexcept {
    return all_closed_.load(std::memory_order_acquire);
  }

  // 12 §7.2 B1–B3. |next| must be built with BuildOptions::base = current
  // topology and its new nodes already opened. Kept producers of removed
  // edges are held for at most one in-flight process call (MUT-3), then
  // every node of |next| routes through |next|; removed edges of kept
  // producers get an EOS marker so old in-flight packets finish on the old
  // path (12 §7.3/§7.4). Returns after the swap; retire progress continues
  // in Tick() and completes through |retire.on_complete|. Once Stop() ran
  // the swap is refused (kCancelled): a topology published into a stopping
  // graph would attach nodes that no EOS/cancel ever reaches. The caller
  // abandons |next| in that case.
  [[nodiscard]] Status Publish(std::shared_ptr<RuntimeTopology> next, RetireRequest retire);

  // Watchdog entry (12 §7.7): retries pending EOS injections, checks drain
  // completion and deadlines. Safe from any thread; cheap when idle.
  void Tick();
  [[nodiscard]] std::size_t pending_retirements() const;

  // 12 §6.1: enqueue if the node can take a slot. Safe from any thread.
  void MarkReady(NodeRuntime& node);

 private:
  class Sink;

  struct Retired {
    RetireRequest request;
    std::chrono::steady_clock::time_point deadline;
    bool fast_applied = false;
    bool timed_out = false;
  };

  void Invoke(NodeRuntime& node);
  void InvokeBody(NodeRuntime& node, const std::shared_ptr<const RuntimeTopology>& topo);
  void RunSource(NodeRuntime& node, const RuntimeTopology& topo);
  // 12 §6.2: block means "stop producing" -- a node is not invoked while any
  // block-policy output edge is full. Marks such edges backpressured so the
  // consumer's Pop re-marks this node.
  bool OutputsBlocked(NodeRuntime& node, const RuntimeTopology& topo);
  void RunProcess(NodeRuntime& node, const RuntimeTopology& topo, InputBinding& binding);
  // Shared by RunProcess/RunAsync/RunSource (docs/16 §3).
  [[nodiscard]] std::optional<InputBatch> AcquireBatch(InputBinding& binding);
  [[nodiscard]] std::optional<ProcessResult> RunProcessCall(NodeRuntime& node, ProcessRequest& req, Sink& sink);
  void RunAsync(NodeRuntime& node, const std::shared_ptr<const RuntimeTopology>& topo,
                InputBinding& binding);
  void ParkAsync(NodeRuntime& node, bool until_zero);
  void FinishNode(NodeRuntime& node);  // flush + EOS out + close (12 §4.4 3–4)
  void AfterEmit(const EmitReport& report);
  void RetryBlocked(NodeRuntime& node);
  // 12 §6.2 parking for synchronous emits: a packet whose block-policy
  // edge was full stays queued per (node, edge) in FIFO order and is
  // re-pushed when the consumer frees a slot. A node with parked output is
  // not invoked until everything parked went through, so edge order equals
  // emit order. Returns true when nothing is left parked for |node|.
  bool RetryParked(NodeRuntime& node);
  [[nodiscard]] bool HasParked(const NodeRuntime& node) const;
  [[nodiscard]] bool HasParked(const NodeRuntime& node, const EdgeChannel& edge) const;
  void Park(NodeRuntime& node, EdgeChannel& edge, PacketRef packet);
  void DropParked(NodeRuntime& node);
  void OnNodeDone(NodeRuntime& node);
  void CloseNode(NodeRuntime& node, bool fast);
  // Fast-retired node with no invocation left: close it (exactly once).
  bool TryCloseCancelled(NodeRuntime& node);
  void CheckAllClosed();
  bool DrainComplete(const Retired& r) const;  // retire_mutex_ held
  bool CompleteRetired();  // returns true if any entry progressed
  void RetryInjections();
  void AttachNodes(const std::vector<NodeRuntimeRef>& nodes);
  // Removes |node| from the live list; true for exactly one caller.
  bool Detach(NodeRuntime& node);
  void WaitQuiescent(NodeRuntime& node);
  std::vector<NodeRuntimeRef> LiveNodes() const;
  std::vector<NodeRuntimeRef> ApplyFastLocked(Retired& r);

  AtomicSharedPtr<RuntimeTopology> topology_;
  ExecutorPool& executor_;
  SchedulerEvents events_;
  AsyncDispatch async_;
  std::atomic<bool> started_{false};
  std::atomic<bool> paused_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> fast_stop_{false};
  std::atomic<bool> all_closed_{false};
  // Serialises the Publish swap (stopping_ check -> AttachNodes) against
  // Stop (LiveNodes snapshot -> stopping_ = true): a swap that lands after
  // Stop snapshotted the live set would attach nodes no EOS/cancel sweep
  // reaches (docs/16 §5).
  std::mutex swap_mutex_;
  // Attached nodes not yet closed/failed; every attached NodeRuntime is
  // kept alive here until done so executor tasks never dangle.
  std::atomic<std::uint32_t> live_count_{0};
  mutable std::mutex live_mutex_;
  std::vector<NodeRuntimeRef> live_nodes_;
  // Pending EOS retries: node -> ports whose EOS hit WOULD_BLOCK.
  std::mutex retry_mutex_;
  std::map<NodeRuntime*, std::set<std::string>> eos_retry_;
  // Parked data packets (12 §6.2): node -> [(edge, packet)] in emit order.
  mutable std::mutex park_mutex_;
  std::mutex park_retry_mutex_;
  std::map<NodeRuntime*, std::vector<std::pair<EdgeChannelRef, PacketRef>>> parked_;
  std::atomic<std::uint32_t> parked_nodes_{0};
  // 12 §7.7 retire bookkeeping.
  mutable std::mutex retire_mutex_;
  std::vector<Retired> retired_;
  struct PendingInject {
    EdgeChannelRef edge;
    PacketRef eos;
  };
  std::vector<PendingInject> pending_inject_;
  std::mutex closed_mutex_;
  std::condition_variable closed_cv_;
  bool closed_signalled_ = false;  // guarded by closed_mutex_
  std::atomic<std::uint32_t> retire_pending_{0};  // == retired_.size(), lock-free peek
  std::atomic<bool> alive_{true};
  std::atomic<std::uint32_t> pending_tasks_{0};
};

}  // namespace ge

#endif
