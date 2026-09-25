#ifndef GE_SRC_MUTATION_COORDINATOR_H_
#define GE_SRC_MUTATION_COORDINATOR_H_

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ge/cpp/mutation_applier.h>
#include <ge/cpp/session.h>

namespace ge {

struct MutationRequest {
  OperationId operation = 0;
  MutationPatch patch;
};

// Prepare never touches the running graph; Publish is the atomic swap;
// operation. Private to ge_core: callers drive it through Session::Apply /
// Session::DryRun / Session::PumpMutations, so the mutation pipeline's
// internals can change without recompiling session.h consumers.
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

}  // namespace ge

#endif
