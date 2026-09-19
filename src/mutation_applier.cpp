#include <ge/cpp/mutation_applier.h>

#include <algorithm>
#include <set>

namespace ge {

MutationApplier::MutationApplier(CapabilityResolver resolver)
    : resolver_(std::move(resolver)) {}

namespace {

void Touch(std::vector<std::string>* list, const std::string& id) {
  if (std::find(list->begin(), list->end(), id) == list->end()) list->push_back(id);
}

// 14 §3.2: without explicit ports only a uniquely matching port is allowed.
Result<std::string> UniquePort(const CapabilityDescriptor* cap, bool input,
                               const std::string& node_id) {
  if (cap == nullptr) {
    return Status::GraphInvalid("chain node '" + node_id +
                                "' needs explicit ports: operator capability unavailable");
  }
  const auto& ports = input ? cap->inputs : cap->outputs;
  std::vector<std::string> candidates;
  for (const PortCapability& p : ports) {
    if (!input || p.required) candidates.push_back(p.name);
  }
  if (candidates.empty() && input) {
    for (const PortCapability& p : ports) candidates.push_back(p.name);
  }
  if (candidates.size() != 1) {
    return Status::GraphInvalid("chain node '" + node_id + "' has " +
                                std::to_string(candidates.size()) + " candidate " +
                                (input ? "input" : "output") + " ports; specify ports explicitly");
  }
  return candidates.front();
}

}  // namespace

Result<CandidateSpec> MutationApplier::Apply(const GraphSpec& base,
                                             const MutationPatch& patch) const {
  CandidateSpec out;
  out.candidate = base;
  if (patch.actions.empty()) return Status::InvalidArgument("mutation has no actions");
  std::size_t index = 0;
  for (const MutationAction& action : patch.actions) {
    if (Status s = ApplyOne(action, patch.remove_policy, &out); !s.ok()) {
      JsonObject ctx;
      ctx.emplace("action_index", JsonValue(static_cast<std::int64_t>(index)));
      ctx.emplace("action_type", JsonValue(std::string(ActionTypeName(action))));
      if (!s.context_json().empty()) {
        if (auto inner = ge::ParseJson(s.context_json()); inner.ok()) {
          ctx.emplace("detail", *inner.value);
        }
      }
      return Status(s.code(), "action[" + std::to_string(index) + "] " +
                                  std::string(ActionTypeName(action)) + ": " + s.message(),
                    s.retryable(), JsonValue(std::move(ctx)).Serialize());
    }
    ++index;
  }
  return out;
}

Status MutationApplier::ApplyOne(const MutationAction& action, RemovePolicy default_policy,
                                 CandidateSpec* out) const {
  return std::visit(
      [&](const auto& a) -> Status {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, AddNodeAction>) {
          if (Status s = out->candidate.AddNode(a.node); !s.ok()) return s;
          out->added_nodes.push_back(a.node.id);
          Touch(&out->touched_nodes, a.node.id);
          return Status::Ok();
        } else if constexpr (std::is_same_v<T, RemoveNodeAction>) {
          if (a.mode == RemoveMode::kBypass) {
            RemoveChainAction chain{{a.node}, a.mode, a.remove_policy};
            return RemoveChain(chain, a.remove_policy.value_or(default_policy), out);
          }
          return RemoveNodeWithEdges(a.node, a.remove_policy.value_or(default_policy), out);
        } else if constexpr (std::is_same_v<T, InsertChainAction>) {
          return InsertChain(a, out);
        } else if constexpr (std::is_same_v<T, RemoveChainAction>) {
          return RemoveChain(a, a.remove_policy.value_or(default_policy), out);
        } else if constexpr (std::is_same_v<T, ReplaceNodeAction>) {
          return ReplaceNode(a, a.remove_policy.value_or(default_policy), out);
        } else if constexpr (std::is_same_v<T, RemoveBranchAction>) {
          return RemoveBranch(a, a.remove_policy.value_or(default_policy), out);
        } else if constexpr (std::is_same_v<T, AddEdgeAction>) {
          return AddEdgeTracked(a.edge, out);
        } else if constexpr (std::is_same_v<T, RemoveEdgeAction>) {
          return RemoveEdgeTracked(a.edge, out);
        } else {
          static_assert(std::is_same_v<T, SetNodeOptionsAction>);
          if (out->candidate.FindNode(a.node) == nullptr) {
            return Status::NotFound("node '" + a.node + "' not found");
          }
          out->parameter_updates.push_back(a);
          Touch(&out->touched_nodes, a.node);
          return Status::Ok();
        }
      },
      action);
}

Status MutationApplier::AddEdgeTracked(EdgeSpec edge, CandidateSpec* out) const {
  if (edge.id.empty()) edge.id = GraphSpec::DeriveEdgeId(edge.from, edge.to);
  const std::string id = edge.id;
  Touch(&out->touched_nodes, edge.from.node_id);
  Touch(&out->touched_nodes, edge.to.node_id);
  if (Status s = out->candidate.AddEdge(std::move(edge)); !s.ok()) return s;
  out->added_edges.push_back(id);
  Touch(&out->touched_edges, id);
  return Status::Ok();
}

Status MutationApplier::RemoveEdgeTracked(const std::string& id, CandidateSpec* out) const {
  const EdgeSpec* e = out->candidate.FindEdge(id);
  if (e == nullptr) return Status::NotFound("edge '" + id + "' not found");
  Touch(&out->touched_nodes, e->from.node_id);
  Touch(&out->touched_nodes, e->to.node_id);
  Touch(&out->touched_edges, id);
  if (Status s = out->candidate.RemoveEdge(id); !s.ok()) return s;
  // An edge added and removed in the same patch never existed.
  if (const auto it = std::find(out->added_edges.begin(), out->added_edges.end(), id);
      it != out->added_edges.end()) {
    out->added_edges.erase(it);
  } else {
    out->removed_edges.push_back(id);
  }
  return Status::Ok();
}

Status MutationApplier::RemoveNodeWithEdges(const std::string& id, RemovePolicy policy,
                                            CandidateSpec* out) const {
  if (out->candidate.FindNode(id) == nullptr) return Status::NotFound("node '" + id + "' not found");
  std::vector<std::string> incident;
  for (const EdgeSpec* e : out->candidate.EdgesFrom(id)) incident.push_back(e->id);
  for (const EdgeSpec* e : out->candidate.EdgesTo(id)) {
    if (std::find(incident.begin(), incident.end(), e->id) == incident.end()) incident.push_back(e->id);
  }
  for (const std::string& eid : incident) {
    if (Status s = RemoveEdgeTracked(eid, out); !s.ok()) return s;
  }
  if (Status s = out->candidate.RemoveNode(id); !s.ok()) return s;
  Touch(&out->touched_nodes, id);
  if (const auto it = std::find(out->added_nodes.begin(), out->added_nodes.end(), id);
      it != out->added_nodes.end()) {
    out->added_nodes.erase(it);
  } else {
    out->removed_nodes.push_back(id);
    out->node_remove_policy[id] = policy;
  }
  return Status::Ok();
}

// 12 §7.3
Status MutationApplier::InsertChain(const InsertChainAction& a, CandidateSpec* out) const {
  if (a.nodes.empty()) return Status::GraphInvalid("insert_chain needs at least one node");
  const EdgeSpec* split = out->candidate.FindEdge(a.edge);
  if (split == nullptr) return Status::NotFound("edge '" + a.edge + "' not found");
  const EdgeSpec old = *split;

  for (const NodeSpec& n : a.nodes) {
    if (out->candidate.FindNode(n.id) != nullptr) {
      return Status::AlreadyExists("chain node id '" + n.id + "' already exists in candidate graph");
    }
  }
  // Resolve chain-internal ports: N_i.out -> N_{i+1}.in.
  struct Ports {
    std::string in;
    std::string out;
  };
  std::vector<Ports> ports(a.nodes.size());
  for (std::size_t i = 0; i < a.nodes.size(); ++i) {
    const CapabilityDescriptor* cap = resolver_ ? resolver_(a.nodes[i].op) : nullptr;
    const bool first = i == 0, last = i + 1 == a.nodes.size();
    if (first && a.ports.chain_input) {
      ports[i].in = *a.ports.chain_input;
    } else {
      auto r = UniquePort(cap, true, a.nodes[i].id);
      if (!r.ok()) return r.status();
      ports[i].in = *r;
    }
    if (last && a.ports.chain_output) {
      ports[i].out = *a.ports.chain_output;
    } else {
      auto r = UniquePort(cap, false, a.nodes[i].id);
      if (!r.ok()) return r.status();
      ports[i].out = *r;
    }
  }

  if (Status s = RemoveEdgeTracked(old.id, out); !s.ok()) return s;
  for (const NodeSpec& n : a.nodes) {
    if (Status s = out->candidate.AddNode(n); !s.ok()) return s;
    out->added_nodes.push_back(n.id);
    Touch(&out->touched_nodes, n.id);
  }
  EdgeSpec head = old;
  head.id.clear();
  head.to = {a.nodes.front().id, ports.front().in};
  if (Status s = AddEdgeTracked(std::move(head), out); !s.ok()) return s;
  for (std::size_t i = 0; i + 1 < a.nodes.size(); ++i) {
    EdgeSpec link;
    link.from = {a.nodes[i].id, ports[i].out};
    link.to = {a.nodes[i + 1].id, ports[i + 1].in};
    link.queue = old.queue;
    if (Status s = AddEdgeTracked(std::move(link), out); !s.ok()) return s;
  }
  EdgeSpec tail = old;
  tail.id.clear();
  tail.from = {a.nodes.back().id, ports.back().out};
  if (Status s = AddEdgeTracked(std::move(tail), out); !s.ok()) return s;
  return Status::Ok();
}

// 12 §7.4
Status MutationApplier::RemoveChain(const RemoveChainAction& a, RemovePolicy policy,
                                    CandidateSpec* out) const {
  if (a.nodes.empty()) return Status::GraphInvalid("remove_chain needs at least one node");
  if (a.mode == RemoveMode::kDisconnect) {
    for (const std::string& id : a.nodes) {
      if (Status s = RemoveNodeWithEdges(id, policy, out); !s.ok()) return s;
    }
    return Status::Ok();
  }
  auto boundary = GraphValidator::CheckLinearChain(out->candidate, a.nodes);
  if (!boundary.ok()) return boundary.status();
  const EdgeSpec in = *out->candidate.FindEdge(boundary->inbound_edge);
  const EdgeSpec outgoing = *out->candidate.FindEdge(boundary->outbound_edge);
  for (const std::string& id : a.nodes) {
    if (Status s = RemoveNodeWithEdges(id, policy, out); !s.ok()) return s;
  }
  EdgeSpec bypass;
  bypass.from = in.from;
  bypass.to = outgoing.to;
  bypass.queue = outgoing.queue;
  bypass.sync = outgoing.sync;
  bypass.feedback = in.feedback || outgoing.feedback;
  return AddEdgeTracked(std::move(bypass), out);
}

// 12 §7.5
Status MutationApplier::ReplaceNode(const ReplaceNodeAction& a, RemovePolicy policy,
                                    CandidateSpec* out) const {
  const NodeSpec* old = out->candidate.FindNode(a.node);
  if (old == nullptr) return Status::NotFound("node '" + a.node + "' not found");
  NodeSpec replacement = *old;
  replacement.op = a.replacement_op;
  replacement.options = a.replacement_options;

  const auto map_port = [&](bool input, const std::string& port) -> Result<std::string> {
    if (a.port_mapping) {
      const auto& m = input ? a.port_mapping->inputs : a.port_mapping->outputs;
      const auto it = m.find(port);
      if (it == m.end()) {
        return Status::GraphInvalid("replace_node: no mapping for connected " +
                                    std::string(input ? "input" : "output") + " port '" + port + "'");
      }
      return it->second;
    }
    if (const CapabilityDescriptor* cap = resolver_ ? resolver_(a.replacement_op) : nullptr) {
      const PortCapability* p = input ? cap->FindInput(port) : cap->FindOutput(port);
      if (p == nullptr) {
        return Status::GraphInvalid("replace_node: replacement '" + a.replacement_op.ToString() +
                                    "' has no " + (input ? "input" : "output") + " port '" + port +
                                    "' and no port_mapping was given");
      }
    }
    return port;
  };

  std::vector<EdgeSpec> incoming, outgoing;
  for (const EdgeSpec* e : out->candidate.EdgesTo(a.node)) incoming.push_back(*e);
  for (const EdgeSpec* e : out->candidate.EdgesFrom(a.node)) {
    if (e->to.node_id != a.node) outgoing.push_back(*e);
  }
  for (EdgeSpec& e : incoming) {
    auto p = map_port(true, e.to.port);
    if (!p.ok()) return p.status();
    e.to.port = *p;
    if (e.from.node_id == a.node) {
      auto q = map_port(false, e.from.port);
      if (!q.ok()) return q.status();
      e.from.port = *q;
    }
  }
  for (EdgeSpec& e : outgoing) {
    auto p = map_port(false, e.from.port);
    if (!p.ok()) return p.status();
    e.from.port = *p;
  }

  if (Status s = RemoveNodeWithEdges(a.node, policy, out); !s.ok()) return s;
  if (Status s = out->candidate.AddNode(replacement); !s.ok()) return s;
  out->added_nodes.push_back(replacement.id);
  for (EdgeSpec& e : incoming) {
    if (Status s = AddEdgeTracked(std::move(e), out); !s.ok()) return s;
  }
  for (EdgeSpec& e : outgoing) {
    if (Status s = AddEdgeTracked(std::move(e), out); !s.ok()) return s;
  }
  return Status::Ok();
}

// 12 §7.6
Status MutationApplier::RemoveBranch(const RemoveBranchAction& a, RemovePolicy policy,
                                     CandidateSpec* out) const {
  if (a.nodes.empty()) return Status::GraphInvalid("remove_branch needs a node selection");
  const std::vector<std::string> entry_ids = a.EntryEdges();
  const std::set<std::string_view> selected(a.nodes.begin(), a.nodes.end());
  std::vector<std::string_view> stack;
  for (const std::string& eid : entry_ids) {
    const EdgeSpec* entry = out->candidate.FindEdge(eid);
    if (entry == nullptr) return Status::NotFound("entry edge '" + eid + "' not found");
    if (!selected.contains(entry->to.node_id)) {
      return Status::GraphInvalid("entry edge '" + eid + "' does not lead into the selection");
    }
    if (selected.contains(entry->from.node_id)) {
      return Status::GraphInvalid("entry edge '" + eid + "' must start outside the selection");
    }
    stack.push_back(entry->to.node_id);
  }
  for (const std::string& id : a.nodes) {
    if (out->candidate.FindNode(id) == nullptr) return Status::NotFound("node '" + id + "' not found");
  }
  // Reachability from the entry edges within the selection (12 §7.6 step 1).
  std::set<std::string_view> reached;
  while (!stack.empty()) {
    const std::string_view n = stack.back();
    stack.pop_back();
    if (!reached.insert(n).second) continue;
    for (const EdgeSpec* e : out->candidate.EdgesFrom(n)) {
      if (selected.contains(e->to.node_id)) stack.push_back(e->to.node_id);
    }
  }
  for (const std::string& id : a.nodes) {
    if (!reached.contains(id)) {
      return Status::GraphInvalid("node '" + id + "' is not reachable from the entry edge(s) inside the selection");
    }
  }
  const auto deps = GraphValidator::FindSharedDependencies(out->candidate, a.nodes, entry_ids);
  if (!deps.empty()) {
    JsonArray arr;
    for (const SharedDependency& d : deps) arr.push_back(d.ToJson());
    JsonObject ctx;
    ctx.emplace("dependencies", JsonValue(std::move(arr)));
    return Status::SharedDependency(
        "branch selection shares " + std::to_string(deps.size()) + " dependency(ies) with the rest of the graph",
        JsonValue(std::move(ctx)).Serialize());
  }
  for (const std::string& id : a.nodes) {
    if (Status s = RemoveNodeWithEdges(id, policy, out); !s.ok()) return s;
  }
  return Status::Ok();
}

}  // namespace ge
