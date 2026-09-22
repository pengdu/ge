#ifndef GE_CPP_MUTATION_APPLIER_H_
#define GE_CPP_MUTATION_APPLIER_H_

#include <map>
#include <string>
#include <vector>

#include <ge/cpp/graph_spec.h>
#include <ge/cpp/graph_validator.h>
#include <ge/cpp/types.h>

namespace ge {

// data transformation: no negotiation, warm-up or resources. The caller runs
// GraphValidator on |candidate| afterwards (A4–A5).
struct CandidateSpec {
  GraphSpec candidate;
  std::vector<std::string> added_nodes;
  std::vector<std::string> removed_nodes;
  std::vector<std::string> added_edges;
  std::vector<std::string> removed_edges;
  std::map<std::string, RemovePolicy> node_remove_policy;
  // returned untouched for the ParameterStore.
  std::vector<SetNodeOptionsAction> parameter_updates;
  std::vector<std::string> touched_nodes;
  std::vector<std::string> touched_edges;
};

class MutationApplier final {
 public:
  // "unique matching input/output port") and ReplaceNode port mapping
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
