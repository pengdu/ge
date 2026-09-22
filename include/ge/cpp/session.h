#ifndef GE_CPP_SESSION_H_
#define GE_CPP_SESSION_H_

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
#include <string>
#include <thread>
#include <vector>

#include <ge/cpp/async_runtime.h>
#include <ge/cpp/graph_diff.h>
#include <ge/cpp/graph_spec.h>
#include <ge/cpp/graph_validator.h>
#include <ge/cpp/mutation_applier.h>
#include <ge/cpp/operation.h>
#include <ge/cpp/operator.h>
#include <ge/cpp/resource_ledger.h>
#include <ge/cpp/runtime_topology.h>
#include <ge/cpp/scheduler.h>
#include <ge/cpp/types.h>

namespace ge {

// 12 §2.4a
enum class SessionState : std::uint8_t {
  kCreated, kStarting, kRunning, kPausing, kPaused, kStopping, kStopped, kFailed
};
[[nodiscard]] std::string_view ToString(SessionState s) noexcept;

struct SessionOptions {
  SessionId id = 1;
  AlignedOptions aligned;
  // 12 §7.7 default drain deadline (transcode branches: 2s).
  std::chrono::milliseconds drain_timeout{2000};
  // 12 §7.1 merge limit for compatible queued mutations.
  std::size_t mutation_merge_limit = 8;
  // true: a dedicated coordinator thread consumes the MutationQueue
  // (12 §2.5). false: the host pumps it with Session::PumpMutations() --
  // deterministic tests with an inline ExecutorPool.
  bool coordinator_thread = true;
  // REL-2 (retries arrive with P5): a node failure fails the session and
  // fast-stops the graph.
  bool fail_session_on_node_failure = true;
  // P5: shared engine AsyncRuntime (null: async operators cannot run).
  AsyncRuntime* async_runtime = nullptr;
  // ASY-7 session-level batching switch.
  bool batching = true;
  // 12 §10 / TD-03: shared engine ledger (null: no admission). Session::Create
  // reserves the initial graph; every mutation reserves its added nodes and
  // edges at Prepare A6 and returns the removed ones when their retire
  // completes. Rejections are RESOURCE_EXHAUSTED with RES-3 context.
  ResourceLedger* resource_ledger = nullptr;
  // 12 §10.3: an edge whose packet bound cannot be derived (audio, bytes,
  // json, custom tags without queue.max_packet_bytes) is rejected at
  // admission instead of merely being left out of the budget.
  bool reject_unbudgeted_edges = false;
};

struct SessionEvents {
  std::function<void(SessionState from, SessionState to)> on_state_changed;
  std::function<void(NodeRuntime&, const Status&)> on_node_failed;
  std::function<void(TopologyVersion published)> on_topology_published;
  std::function<void(TopologyVersion retired_version)> on_drain_timeout;
  // P6: builtin operator event (ProcessRequest::events); executor thread.
  std::function<void(NodeRuntime&, std::string type, Severity, JsonValue detail)> on_operator_event;
};

// 12 §7.1 MutationRequest.
struct MutationRequest {
  OperationId operation = 0;
  MutationPatch patch;
};

// MUT-7 dry-run: A2–A5 without touching the running graph.
struct DryRunResult {
  GraphSpec candidate;
  ValidatedGraph validated;
  // Structural diff against the base version: what the runtime would keep,
  // rebind, recreate or retire (12 §7.2 A5').
  GraphDiff diff;
  [[nodiscard]] JsonValue ToJson() const;
};

// 13 §6.3
struct ParameterUpdate {
  ParameterVersion version = 0;
  OperationId operation = 0;
};

class Session;

// 12 §7 / 13 §7.1: serial two-phase mutation orchestration for one Session.
// Prepare never touches the running graph; Publish is the atomic swap;
// Retire progress is driven by the Scheduler (12 §7.7) and completes the
// operation.
class MutationCoordinator final {
 public:
  MutationCoordinator(Session& session, OperatorFactory& factory, OperationRegistry& operations);
  ~MutationCoordinator();

  // Enqueue (FIFO). The operation is already registered as accepted.
  void Submit(MutationRequest request);
  // Processes the queue head (merging compatible successors, 12 §7.1) on the
  // calling thread. Returns false if the queue was empty. Must not be called
  // from an executor thread.
  bool Pump();
  void StartThread();
  void StopThread();
  // Rejects every queued request (session stopping).
  void CancelAll(std::string reason);
  [[nodiscard]] Result<DryRunResult> DryRun(const RuntimeTopology& base, const MutationPatch& patch) const;

 private:
  struct Merged {
    std::vector<MutationRequest> origins;
    MutationPatch patch;  // concatenated actions
  };
  struct Prepared {
    std::shared_ptr<RuntimeTopology> candidate;
    CandidateSpec changes;
    GraphDiff diff;
    RemovePolicy policy = RemovePolicy::kDrain;
    // A6: estimate of the nodes/edges this version adds (reserved before
    // warm-up, returned if warm-up or publish fails) and of the ones it
    // removes (returned when the retire completes).
    std::vector<ResourceAmount> added;
    std::vector<ResourceAmount> removed;
  };

  Merged TakeBatch();  // queue_mutex_ held by caller? no: locks internally
  void Execute(Merged batch);
  [[nodiscard]] Result<CandidateSpec> ApplyPatch(const RuntimeTopology& base,
                                                 const MutationPatch& patch) const;
  [[nodiscard]] Result<Prepared> Prepare(const RuntimeTopology& base, const MutationPatch& patch,
                                         TopologyVersion version);
  void MigrateParameters(const RuntimeTopology& base, const MutationPatch& patch,
                         GraphSpec* candidate) const;
  [[nodiscard]] static bool Disjoint(const CandidateSpec& a, const CandidateSpec& b);
  void FailAll(const Merged& batch, const Status& status, const std::string& action_detail);
  void Loop();

  Session& session_;
  OperatorFactory& factory_;
  OperationRegistry& operations_;
  MutationApplier applier_;
  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<MutationRequest> queue_;
  std::thread thread_;
  bool stop_thread_ = false;  // guarded by queue_mutex_
  std::mutex execute_mutex_;  // one Execute at a time (Pump vs thread)
};

// 12 §2.5 Session: atomic current topology, state machine (12 §2.4a),
// MutationQueue, RetiredTopologyList (inside Scheduler) and parameter
// updates (12 §5).
class Session final {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Session>> Create(
      const GraphSpec& spec, OperatorFactory& factory, ExecutorPool& executor,
      OperationRegistry& operations, SessionOptions options = {}, SessionEvents events = {});
  ~Session();

  [[nodiscard]] SessionId id() const noexcept { return options_.id; }
  [[nodiscard]] SessionState state() const noexcept { return state_.load(std::memory_order_acquire); }
  [[nodiscard]] std::shared_ptr<RuntimeTopology> current_topology() const {
    return scheduler_.topology();
  }
  [[nodiscard]] TopologyVersion topology_version() const { return current_topology()->version(); }
  [[nodiscard]] const Status& failure() const noexcept { return failure_; }

  // Lifecycle (12 §2.4a). Start opens every node (12 §7.2 A7 semantics for
  // the initial graph) then runs.
  [[nodiscard]] Status Start();
  [[nodiscard]] Status Pause();
  [[nodiscard]] Status Resume();
  // fast=false drains (EOS at sources), fast=true cancels. The operation
  // succeeds once every node is closed.
  [[nodiscard]] Result<OperationId> Stop(bool fast, CallerContext caller = {});
  [[nodiscard]] bool WaitStopped(std::chrono::milliseconds timeout);

  // Mutation (12 §7). Accepted only in running/paused.
  [[nodiscard]] Result<OperationId> Apply(MutationPatch patch, CallerContext caller = {});
  [[nodiscard]] Result<DryRunResult> DryRun(const MutationPatch& patch) const;
  // Host-side pump when SessionOptions::coordinator_thread == false.
  bool PumpMutations() { return coordinator_.Pump(); }

  // Parameters (12 §5, PAR-2..7). Accepted only in running/paused.
  [[nodiscard]] Result<ParameterUpdate> SetParameters(std::string_view node_id, JsonValue parameters,
                                                      CallerContext caller = {});

  // OBS-3 snapshot: state, version, nodes, edges, contracts, parameters.
  [[nodiscard]] JsonValue Snapshot() const;

  // Watchdog entry (12 §7.7): drives drain deadlines and EOS retries.
  void Tick() { scheduler_.Tick(); }
  [[nodiscard]] CompletionSink* completion_sink() const noexcept { return sink_.get(); }

  // Internal (coordinator).
  [[nodiscard]] Scheduler& scheduler() noexcept { return scheduler_; }
  [[nodiscard]] const Scheduler& scheduler() const noexcept { return scheduler_; }
  [[nodiscard]] const SessionOptions& options() const noexcept { return options_; }
  // 12 §10 admission (RES-1..4). One lease per session holds the running
  // sum of every live node/edge estimate; Prepare A6 grows it by the
  // candidate's additions (all-or-nothing, RESOURCE_EXHAUSTED with RES-3
  // context on failure) and a completed retire shrinks it by the removals.
  // No ledger configured: both are no-ops.
  [[nodiscard]] Status ReserveResources(const std::vector<ResourceAmount>& delta, std::string_view purpose);
  void ReturnResources(const std::vector<ResourceAmount>& delta);
  [[nodiscard]] std::vector<ResourceAmount> HeldResources() const;
  [[nodiscard]] SessionEvents& events() noexcept { return events_; }
  [[nodiscard]] bool AcceptsMutations() const noexcept {
    const SessionState s = state();
    return s == SessionState::kRunning || s == SessionState::kPaused;
  }
  [[nodiscard]] Status NotMutable() const;

 private:
  Session(std::shared_ptr<RuntimeTopology> topology, OperatorFactory& factory,
          ExecutorPool& executor, OperationRegistry& operations, SessionOptions options,
          SessionEvents events);
  void Transition(SessionState to);
  bool TransitionIf(SessionState from, SessionState to);
  void OnAllClosed();
  void OnNodeFailed(NodeRuntime& node, const Status& status);

  SessionOptions options_;
  SessionEvents events_;
  OperationRegistry& operations_;
  // Declared before the scheduler: retire callbacks return resources from
  // scheduler threads and must find the lease alive.
  mutable std::mutex lease_mutex_;
  ResourceLease lease_;  // guarded by lease_mutex_; amounts() == everything live
  Scheduler scheduler_;
  MutationCoordinator coordinator_;
  std::shared_ptr<SessionCompletionSink> sink_;
  std::atomic<SessionState> state_{SessionState::kCreated};
  Status failure_;
  std::mutex stop_mutex_;
  std::vector<OperationId> stop_waiters_;  // guarded by stop_mutex_
  bool stopped_signalled_ = false;         // guarded by stop_mutex_
  std::condition_variable stop_cv_;
};

}  // namespace ge

#endif
