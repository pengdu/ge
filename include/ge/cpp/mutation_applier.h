#ifndef GE_CPP_MUTATION_APPLIER_H_
#define GE_CPP_MUTATION_APPLIER_H_

#include <map>
#include <string>
#include <vector>

#include <ge/cpp/graph_spec.h>
#include <ge/cpp/graph_validator.h>
#include <ge/cpp/types.h>

namespace ge {

// Result of applying a MutationPatch to a GraphSpec (12 §7.2 A2–A3). Pure
// data transformation: no negotiation, warm-up or resources. The caller runs
// GraphValidator on |candidate| afterwards (A4–A5).
struct CandidateSpec {
  GraphSpec candidate;
  std::vector<std::string> added_nodes;
  std::vector<std::string> removed_nodes;
  std::vector<std::string> added_edges;
  std::vector<std::string> removed_edges;
  // node id -> per-action remove policy override (12 §7.7).
  std::map<std::string, RemovePolicy> node_remove_policy;
  // set_node_options actions are not topology changes (14 §3.6); they are
  // returned untouched for the ParameterStore.
  std::vector<SetNodeOptionsAction> parameter_updates;
  // Every node/edge/port touched, for MutationQueue merge checks (12 §7.1).
  std::vector<std::string> touched_nodes;
  std::vector<std::string> touched_edges;
};

class MutationApplier final {
 public:
  // |resolver| is needed for InsertChain without explicit ports (14 §3.2:
  // "unique matching input/output port") and ReplaceNode port mapping
  // (12 §7.5). It may be empty if every action is explicit.
  explicit MutationApplier(CapabilityResolver resolver = {});

  [[nodiscard]] Result<CandidateSpec> Apply(const GraphSpec& base,
                                            const MutationPatch& patch) const;

 private:
  Status ApplyOne(const MutationAction& action, RemovePolicy default_policy,
                  CandidateSpec* out) const;
  Status InsertChain(const InsertChainAction& a, CandidateSpec* out) const;
  Status RemoveChain(const RemoveChainAction& a, RemovePolicy policy, CandidateSpec* out) const;
  Status ReplaceNode(const ReplaceNodeAction& a, RemovePolicy policy, CandidateSpec* out) const;
  Status RemoveBranch(const RemoveBranchAction& a, RemovePolicy policy, CandidateSpec* out) const;
  Status RemoveNodeWithEdges(const std::string& id, RemovePolicy policy, CandidateSpec* out) const;
  Status AddEdgeTracked(EdgeSpec edge, CandidateSpec* out) const;
  Status RemoveEdgeTracked(const std::string& id, CandidateSpec* out) const;

  CapabilityResolver resolver_;
};

}  // namespace ge

#endif
