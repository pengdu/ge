#ifndef GE_CPP_GRAPH_DIFF_H_
#define GE_CPP_GRAPH_DIFF_H_

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ge/cpp/graph_spec.h>
#include <ge/cpp/graph_validator.h>
#include <ge/cpp/json.h>

namespace ge {

// Structural diff between the running topology (base spec + negotiated
// contracts) and a validated candidate (12 §7.2 A5'). Pure function of its
// inputs, independent of which patch actions produced the candidate: the
// runtime decides reuse vs. recreate from this and nothing else, so the
// answer is inspectable (dry-run, operation detail) before anything runs.
//
// Node classification is by external id:
//   kAdded     absent from base
//   kRemoved   absent from candidate
//   kReplaced  same id, different operator@version (replace_node)
//   kUpdated   same operator, but inputs re-wired, port set, executor or
//              parallelism changed: the NodeRuntime is kept, its binding is
//              rebound at publish
//   kKept      untouched (options-only changes are not topology changes)
// Edge classification is by edge id:
//   kAdded / kRemoved as above
//   kRecreated same id but a different data-plane contract (12 §8.3 fan-out
//              re-negotiation) or a different endpoint/queue: new channel,
//              queued packets of the old one finish on the old path
//   kKept      same channel object, packets untouched (MUT-3)
struct GraphDiff {
  enum class NodeChange : std::uint8_t { kKept, kAdded, kRemoved, kReplaced, kUpdated };
  enum class EdgeChange : std::uint8_t { kKept, kAdded, kRemoved, kRecreated };

  struct NodeEntry {
    NodeChange change = NodeChange::kKept;
    std::string reason;  // human-readable, for kUpdated / kReplaced / kRecreated
  };
  struct EdgeEntry {
    EdgeChange change = EdgeChange::kKept;
    std::string reason;
  };

  std::map<std::string, NodeEntry> nodes;
  std::map<std::string, EdgeEntry> edges;

  [[nodiscard]] std::vector<std::string> Nodes(NodeChange c) const;
  [[nodiscard]] std::vector<std::string> Edges(EdgeChange c) const;
  // Nodes whose NodeRuntime is carried over (kKept + kUpdated).
  [[nodiscard]] std::set<std::string> ReusedNodes() const;
  // Edges whose EdgeChannel is carried over (kKept).
  [[nodiscard]] std::set<std::string> ReusedEdges() const;
  // True when nothing structural changed (parameter-only patch).
  [[nodiscard]] bool Empty() const;
  [[nodiscard]] JsonValue ToJson() const;

  // |base_contracts| / |candidate_contracts| are the negotiated edge
  // contracts of each side (ValidatedGraph::edge_contracts).
  [[nodiscard]] static GraphDiff Compute(const GraphSpec& base,
                                         const std::map<std::string, ConnectionContract>& base_contracts,
                                         const GraphSpec& candidate,
                                         const std::map<std::string, ConnectionContract>& candidate_contracts);
};

[[nodiscard]] std::string_view ToString(GraphDiff::NodeChange c) noexcept;
[[nodiscard]] std::string_view ToString(GraphDiff::EdgeChange c) noexcept;

}  // namespace ge

#endif
