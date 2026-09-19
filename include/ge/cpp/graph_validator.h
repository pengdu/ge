#ifndef GE_CPP_GRAPH_VALIDATOR_H_
#define GE_CPP_GRAPH_VALIDATOR_H_

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <ge/cpp/capability.h>
#include <ge/cpp/graph_spec.h>
#include <ge/cpp/types.h>

namespace ge {

// Resolves an operator key to its capability descriptor. Backed by the
// PluginRegistry in production; tests supply an in-memory table.
using CapabilityResolver =
    std::function<const CapabilityDescriptor*(const OperatorKey&)>;

struct ValidationIssue {
  ge_status_code code = GE_STATUS_GRAPH_INVALID;
  std::string message;
  std::string node_id;  // may be empty
  std::string edge_id;  // may be empty
  [[nodiscard]] JsonValue ToJson() const;
};

// Everything the control plane needs after a successful validation:
// per-edge contracts, per-output fan-out contracts and a topological order.
struct ValidatedGraph {
  // edge id -> contract (identical for all edges of one fan-out).
  std::map<std::string, ConnectionContract> edge_contracts;
  // "node.port" -> contract shared by all consumers of that output.
  std::map<std::string, ConnectionContract> output_contracts;
  // Node ids in a topological order that ignores feedback edges.
  std::vector<std::string> topological_order;
  // Nodes without any incoming (non-feedback) edge.
  std::vector<std::string> sources;
  // Nodes without any outgoing edge.
  std::vector<std::string> sinks;
};

// Shared dependency report (12 §7.6 step 3, 14 §3.5).
struct SharedDependency {
  std::string node_id;       // node inside the selection
  std::string edge_id;       // edge crossing the selection boundary
  std::string external_node; // node outside the selection
  [[nodiscard]] JsonValue ToJson() const;
};

struct ValidateOptions {
  PreferenceSet preferences;
  // Stop after the first issue. Default collects every issue (12 §11).
  bool fail_fast = false;
};

class GraphValidator final {
 public:
  explicit GraphValidator(CapabilityResolver resolver,
                          CapabilityNegotiator* negotiator = nullptr);

  // 15 P1 task 4: port existence, direction, required inputs, cycles,
  // feedback edges, unique external ids, then negotiates every edge
  // (single or fan-out) into a ConnectionContract.
  [[nodiscard]] Result<ValidatedGraph> Validate(const GraphSpec& spec,
                                                const ValidateOptions& options = {});

  // Issues collected by the last Validate() call (empty on success).
  [[nodiscard]] const std::vector<ValidationIssue>& issues() const noexcept {
    return issues_;
  }

  // 12 §7.6: nodes/edges in |selection| must have all incident edges inside
  // the selection except |entry_edge| (which may be empty for chains).
  // Returns the crossing dependencies; empty means the selection is exclusive.
  [[nodiscard]] static std::vector<SharedDependency> FindSharedDependencies(
      const GraphSpec& spec, const std::vector<std::string>& selection,
      std::string_view entry_edge = {});
  // P6: several entry edges (12 §7.6 multi-entry branch).
  [[nodiscard]] static std::vector<SharedDependency> FindSharedDependencies(
      const GraphSpec& spec, const std::vector<std::string>& selection,
      const std::vector<std::string>& entry_edges);

  // 12 §7.4 bypass precondition: |chain| is a linear path with exactly one
  // external input into chain.front() and one external output from
  // chain.back(). Returns those two edges on success.
  struct ChainBoundary {
    std::string inbound_edge;
    std::string outbound_edge;
  };
  [[nodiscard]] static Result<ChainBoundary> CheckLinearChain(
      const GraphSpec& spec, const std::vector<std::string>& chain);

 private:
  CapabilityResolver resolver_;
  CapabilityNegotiator* negotiator_;
  CapabilityNegotiator owned_negotiator_;
  std::vector<ValidationIssue> issues_;
};

}  // namespace ge

#endif
