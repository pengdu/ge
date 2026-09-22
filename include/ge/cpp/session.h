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

enum class SessionState : std::uint8_t {
  kCreated, kStarting, kRunning, kPausing, kPaused, kStopping, kStopped, kFailed
};
[[nodiscard]] std::string_view ToString(SessionState s) noexcept;

struct SessionOptions {
  SessionId id = 1;
  AlignedOptions aligned;
  std::chrono::milliseconds drain_timeout{2000};
  std::size_t mutation_merge_limit = 8;
  // true: a dedicated coordinator thread consumes the MutationQueue
  // deterministic tests with an inline ExecutorPool.
  bool coordinator_thread = true;
  // fast-stops the graph.
  bool fail_session_on_node_failure = true;
  AsyncRuntime* async_runtime = nullptr;
  // ASY-7 session-level batching switch.
  bool batching = true;
  // reserves the initial graph; every mutation reserves its added nodes and
  // edges at Prepare A6 and returns the removed ones when their retire
  // completes. Rejections are RESOURCE_EXHAUSTED with RES-3 context.
  ResourceLedger* resource_ledger = nullptr;
  // json, custom tags without queue.max_packet_bytes) is rejected at
  // admission instead of merely being left out of the budget.
  bool reject_unbudgeted_edges = false;
  // GM-3: validation/negotiation result of the spec's skeleton, produced by
  // GraphTemplate::Prevalidate. When set, Session::Create skips A4/A5 (the
  // caller guarantees the spec differs from the validated skeleton only in
  // node options, which validation does not read). A6 still runs.
  std::shared_ptr<const ValidatedGraph> prevalidated;
};

struct SessionEvents {
  std::function<void(SessionState from, SessionState to)> on_state_changed;
  std::function<void(NodeRuntime&, const Status&)> on_node_failed;
  std::function<void(TopologyVersion published)> on_topology_published;
  std::function<void(TopologyVersion retired_version)> on_drain_timeout;
  std::function<void(NodeRuntime&, std::string type, Severity, JsonValue detail)> on_operator_event;
};

struct MutationRequest {
  OperationId operation = 0;
  MutationPatch patch;
};

// MUT-7 dry-run: A2–A5 without touching the running graph.
struct DryRunResult {
  GraphSpec candidate;
  ValidatedGraph validated;
  // Structural diff against the base version: what the runtime would keep,
  GraphDiff diff;
  [[nodiscard]] JsonValue ToJson() const;
};

struct ParameterUpdate {
  ParameterVersion version = 0;
  OperationId operation = 0;
};

class Session;

// Prepare never touches the running graph; Publish is the atomic swap;
// operation.
class MutationCoordinator final {
 public:
  MutationCoordinator(Session& session, OperatorFactory& factory, OperationRegistry& operations);
  ~MutationCoordinator();

  // Enqueue (FIFO). The operation is already registered as accepted.
  void Submit(MutationRequest request);
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

// MutationQueue, RetiredTopologyList (inside Scheduler) and parameter
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

  // the initial graph) then runs.
  [[nodiscard]] Status Start();
  [[nodiscard]] Status Pause();
  [[nodiscard]] Status Resume();
  // fast=false drains (EOS at sources), fast=true cancels. The operation
  // succeeds once every node is closed.
  [[nodiscard]] Result<OperationId> Stop(bool fast, CallerContext caller = {});
  [[nodiscard]] bool WaitStopped(std::chrono::milliseconds timeout);

  [[nodiscard]] Result<OperationId> Apply(MutationPatch patch, CallerContext caller = {});
  [[nodiscard]] Result<DryRunResult> DryRun(const MutationPatch& patch) const;
  // Host-side pump when SessionOptions::coordinator_thread == false.
  bool PumpMutations() { return coordinator_.Pump(); }

  [[nodiscard]] Result<ParameterUpdate> SetParameters(std::string_view node_id, JsonValue parameters,
                                                      CallerContext caller = {});
  [[nodiscard]] JsonValue Snapshot() const;

  void Tick() { scheduler_.Tick(); }
  [[nodiscard]] CompletionSink* completion_sink() const noexcept { return sink_.get(); }

  // Internal (coordinator).
  [[nodiscard]] Scheduler& scheduler() noexcept { return scheduler_; }
  [[nodiscard]] const Scheduler& scheduler() const noexcept { return scheduler_; }
  [[nodiscard]] const SessionOptions& options() const noexcept { return options_; }
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
