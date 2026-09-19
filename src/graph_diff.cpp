// Structural diff between two graph specs (docs/12 §7.2 A5').
#include <ge/cpp/graph_diff.h>

#include <algorithm>

namespace ge {

std::string_view ToString(GraphDiff::NodeChange c) noexcept {
  switch (c) {
    case GraphDiff::NodeChange::kKept: return "kept";
    case GraphDiff::NodeChange::kAdded: return "added";
    case GraphDiff::NodeChange::kRemoved: return "removed";
    case GraphDiff::NodeChange::kReplaced: return "replaced";
    case GraphDiff::NodeChange::kUpdated: return "updated";
  }
  return "?";
}

std::string_view ToString(GraphDiff::EdgeChange c) noexcept {
  switch (c) {
    case GraphDiff::EdgeChange::kKept: return "kept";
    case GraphDiff::EdgeChange::kAdded: return "added";
    case GraphDiff::EdgeChange::kRemoved: return "removed";
    case GraphDiff::EdgeChange::kRecreated: return "recreated";
  }
  return "?";
}

namespace {

// The input edges of |node| in |spec|, port -> edge id (feedback edges
// included: a re-wired feedback still changes what the binding reads).
std::map<std::string, std::string> Inputs(const GraphSpec& spec, std::string_view node) {
  std::map<std::string, std::string> in;
  for (const EdgeSpec& e : spec.edges()) {
    if (e.to.node_id == node) in[e.to.port] = e.id;
  }
  return in;
}

std::string Join(const std::vector<std::string>& parts) {
  std::string out;
  for (const std::string& p : parts) out += (out.empty() ? "" : ", ") + p;
  return out;
}

}  // namespace

GraphDiff GraphDiff::Compute(const GraphSpec& base,
                             const std::map<std::string, ConnectionContract>& base_contracts,
                             const GraphSpec& candidate,
                             const std::map<std::string, ConnectionContract>& candidate_contracts) {
  GraphDiff d;

  // Replaced nodes first: an edge bound to a replaced NodeRuntime cannot be
  // kept even if its spec is byte-identical (the channel's producer/consumer
  // binding is the old operator instance).
  std::set<std::string> replaced;
  for (const NodeSpec& b : base.nodes()) {
    if (const NodeSpec* c = candidate.FindNode(b.id); c != nullptr && b.op != c->op) replaced.insert(b.id);
  }

  // Edges next: node "updated" depends on which of its input edges survive.
  for (const EdgeSpec& b : base.edges()) {
    const EdgeSpec* c = candidate.FindEdge(b.id);
    if (c == nullptr) {
      d.edges[b.id] = {EdgeChange::kRemoved, {}};
      continue;
    }
    std::vector<std::string> why;
    if (b.from != c->from || b.to != c->to) why.push_back("endpoints");
    if (replaced.contains(c->from.node_id)) why.push_back("producer replaced");
    if (replaced.contains(c->to.node_id)) why.push_back("consumer replaced");
    if (b.queue != c->queue) why.push_back("queue");
    if (b.feedback != c->feedback) why.push_back("feedback");
    const auto bc = base_contracts.find(b.id);
    const auto cc = candidate_contracts.find(b.id);
    if (bc != base_contracts.end() && cc != candidate_contracts.end() &&
        !bc->second.SameDataPlane(cc->second)) {
      why.push_back("contract");
    }
    d.edges[b.id] = why.empty() ? EdgeEntry{EdgeChange::kKept, {}}
                                : EdgeEntry{EdgeChange::kRecreated, Join(why)};
  }
  for (const EdgeSpec& c : candidate.edges()) {
    if (base.FindEdge(c.id) == nullptr) d.edges[c.id] = {EdgeChange::kAdded, {}};
  }

  for (const NodeSpec& b : base.nodes()) {
    const NodeSpec* c = candidate.FindNode(b.id);
    if (c == nullptr) {
      d.nodes[b.id] = {NodeChange::kRemoved, {}};
      continue;
    }
    if (b.op != c->op) {
      d.nodes[b.id] = {NodeChange::kReplaced, b.op.ToString() + " -> " + c->op.ToString()};
      continue;
    }
    std::vector<std::string> why;
    if (b.executor != c->executor) why.push_back("executor");
    if (b.parallelism != c->parallelism) why.push_back("parallelism");
    const auto bin = Inputs(base, b.id);
    const auto cin = Inputs(candidate, b.id);
    for (const auto& [port, edge] : cin) {
      const auto old = bin.find(port);
      if (old == bin.end()) {
        why.push_back("input '" + port + "' added");
      } else if (old->second != edge) {
        why.push_back("input '" + port + "' rewired");
      } else if (const auto e = d.edges.find(edge);
                 e != d.edges.end() && e->second.change == EdgeChange::kRecreated) {
        why.push_back("input '" + port + "' recreated");
      }
    }
    for (const auto& [port, _] : bin) {
      if (!cin.contains(port)) why.push_back("input '" + port + "' removed");
    }
    d.nodes[b.id] = why.empty() ? NodeEntry{NodeChange::kKept, {}}
                                : NodeEntry{NodeChange::kUpdated, Join(why)};
  }
  for (const NodeSpec& c : candidate.nodes()) {
    if (base.FindNode(c.id) == nullptr) d.nodes[c.id] = {NodeChange::kAdded, {}};
  }
  return d;
}

std::vector<std::string> GraphDiff::Nodes(NodeChange c) const {
  std::vector<std::string> out;
  for (const auto& [id, e] : nodes) {
    if (e.change == c) out.push_back(id);
  }
  return out;
}

std::vector<std::string> GraphDiff::Edges(EdgeChange c) const {
  std::vector<std::string> out;
  for (const auto& [id, e] : edges) {
    if (e.change == c) out.push_back(id);
  }
  return out;
}

std::set<std::string> GraphDiff::ReusedNodes() const {
  std::set<std::string> out;
  for (const auto& [id, e] : nodes) {
    if (e.change == NodeChange::kKept || e.change == NodeChange::kUpdated) out.insert(id);
  }
  return out;
}

std::set<std::string> GraphDiff::ReusedEdges() const {
  std::set<std::string> out;
  for (const auto& [id, e] : edges) {
    if (e.change == EdgeChange::kKept) out.insert(id);
  }
  return out;
}

bool GraphDiff::Empty() const {
  return std::all_of(nodes.begin(), nodes.end(),
                     [](const auto& kv) { return kv.second.change == NodeChange::kKept; }) &&
         std::all_of(edges.begin(), edges.end(),
                     [](const auto& kv) { return kv.second.change == EdgeChange::kKept; });
}

JsonValue GraphDiff::ToJson() const {
  JsonObject o;
  JsonObject ns;
  for (const auto& [id, e] : nodes) {
    if (e.change == NodeChange::kKept) continue;
    JsonObject n;
    n["change"] = JsonValue(std::string(ToString(e.change)));
    if (!e.reason.empty()) n["reason"] = JsonValue(e.reason);
    ns[id] = JsonValue(std::move(n));
  }
  JsonObject es;
  for (const auto& [id, e] : edges) {
    if (e.change == EdgeChange::kKept) continue;
    JsonObject n;
    n["change"] = JsonValue(std::string(ToString(e.change)));
    if (!e.reason.empty()) n["reason"] = JsonValue(e.reason);
    es[id] = JsonValue(std::move(n));
  }
  o["nodes"] = JsonValue(std::move(ns));
  o["edges"] = JsonValue(std::move(es));
  return JsonValue(std::move(o));
}

}  // namespace ge
