#include <ge/cpp/graph_validator.h>

#include <algorithm>
#include <set>
#include <unordered_map>

namespace ge {

JsonValue ValidationIssue::ToJson() const {
  JsonObject o;
  o.emplace("code", JsonValue(static_cast<std::int64_t>(code)));
  o.emplace("message", JsonValue(message));
  if (!node_id.empty()) o.emplace("node", JsonValue(node_id));
  if (!edge_id.empty()) o.emplace("edge", JsonValue(edge_id));
  return JsonValue(std::move(o));
}

JsonValue SharedDependency::ToJson() const {
  JsonObject o;
  o.emplace("node", JsonValue(node_id));
  o.emplace("edge", JsonValue(edge_id));
  o.emplace("external_node", JsonValue(external_node));
  return JsonValue(std::move(o));
}

GraphValidator::GraphValidator(CapabilityResolver resolver,
                               CapabilityNegotiator* negotiator)
    : resolver_(std::move(resolver)),
      negotiator_(negotiator != nullptr ? negotiator : &owned_negotiator_) {}

namespace {

struct NodeInfo {
  const NodeSpec* spec = nullptr;
  const CapabilityDescriptor* cap = nullptr;
  std::size_t index = 0;
};

Status IssuesToStatus(const std::vector<ValidationIssue>& issues) {
  // The first issue decides the code; every issue goes into context_json.
  JsonArray arr;
  for (const ValidationIssue& i : issues) arr.push_back(i.ToJson());
  JsonObject ctx;
  ctx.emplace("issues", JsonValue(std::move(arr)));
  const std::string context = JsonValue(std::move(ctx)).Serialize();
  std::string message = issues.front().message;
  if (issues.size() > 1) {
    message += " (+" + std::to_string(issues.size() - 1) + " more)";
  }
  return Status(issues.front().code, std::move(message), false, context);
}

}  // namespace

Result<ValidatedGraph> GraphValidator::Validate(const GraphSpec& spec,
                                                const ValidateOptions& options) {
  issues_.clear();
  ValidatedGraph out;
  const auto issue = [&](ge_status_code code, std::string msg, std::string node = {},
                         std::string edge = {}) {
    issues_.push_back({code, std::move(msg), std::move(node), std::move(edge)});
  };
  const auto stop = [&] { return options.fail_fast && !issues_.empty(); };

  // 1. Nodes: ids, operator resolution, executor/parallelism vs capability.
  std::unordered_map<std::string_view, NodeInfo> nodes;
  if (spec.nodes().empty()) {
    issue(GE_STATUS_GRAPH_INVALID, "graph '" + spec.name() + "' has no nodes");
  }
  for (std::size_t i = 0; i < spec.nodes().size(); ++i) {
    const NodeSpec& n = spec.nodes()[i];
    if (!IsValidStableId(n.id)) {
      issue(GE_STATUS_GRAPH_INVALID, "invalid node id '" + n.id + "'", n.id);
    }
    if (nodes.contains(n.id)) {
      issue(GE_STATUS_GRAPH_INVALID, "duplicate node id '" + n.id + "'", n.id);
      continue;
    }
    const CapabilityDescriptor* cap = resolver_ ? resolver_(n.op) : nullptr;
    if (cap == nullptr) {
      issue(GE_STATUS_NOT_FOUND, "node '" + n.id + "' uses unknown operator '" + n.op.ToString() + "'",
            n.id);
    } else {
      const auto has_device = [&](DeviceKind k) {
        return std::find(cap->execution.devices.begin(), cap->execution.devices.end(), k) !=
               cap->execution.devices.end();
      };
      if (n.executor.kind == ExecutorKind::kGpu && !cap->execution.devices.empty() &&
          !has_device(DeviceKind::kGpu)) {
        issue(GE_STATUS_GRAPH_INVALID, "node '" + n.id + "' requests gpu executor but operator '" +
                                           n.op.ToString() + "' does not support gpu", n.id);
      }
      if (n.executor.kind == ExecutorKind::kCpu && !cap->execution.devices.empty() &&
          !has_device(DeviceKind::kCpu)) {
        issue(GE_STATUS_GRAPH_INVALID, "node '" + n.id + "' requests cpu executor but operator '" +
                                           n.op.ToString() + "' does not support cpu", n.id);
      }
      if (n.parallelism > cap->execution.max_parallelism) {
        issue(GE_STATUS_GRAPH_INVALID,
              "node '" + n.id + "' parallelism " + std::to_string(n.parallelism) +
                  " exceeds operator max_parallelism " +
                  std::to_string(cap->execution.max_parallelism), n.id);
      }
    }
    nodes.emplace(n.id, NodeInfo{&n, cap, i});
    if (stop()) return IssuesToStatus(issues_);
  }

  // 2. Edges: endpoints, ports, direction, cardinality, feedback rules.
  struct OutputUse {
    const PortCapability* port = nullptr;
    std::vector<const EdgeSpec*> consumers;
  };
  std::map<std::string, OutputUse> outputs;  // "node.port"
  std::map<std::string, std::vector<const EdgeSpec*>> inputs;  // "node.port"
  std::set<std::string> edge_ids;
  for (const EdgeSpec& e : spec.edges()) {
    if (e.id.empty() || !edge_ids.insert(e.id).second) {
      issue(GE_STATUS_GRAPH_INVALID, "duplicate or empty edge id '" + e.id + "'", {}, e.id);
    }
    const auto from = nodes.find(e.from.node_id);
    const auto to = nodes.find(e.to.node_id);
    if (from == nodes.end()) {
      issue(GE_STATUS_GRAPH_INVALID, "edge '" + e.id + "' from unknown node '" + e.from.node_id + "'",
            {}, e.id);
    }
    if (to == nodes.end()) {
      issue(GE_STATUS_GRAPH_INVALID, "edge '" + e.id + "' to unknown node '" + e.to.node_id + "'",
            {}, e.id);
    }
    if (e.from.node_id == e.to.node_id && !e.feedback) {
      issue(GE_STATUS_GRAPH_INVALID, "edge '" + e.id + "' is a self-loop without feedback=true",
            e.from.node_id, e.id);
    }
    if (e.feedback && e.queue.policy == DropPolicy::kBlock) {
      issue(GE_STATUS_GRAPH_INVALID, "feedback edge '" + e.id + "' must use a non-blocking queue policy",
            {}, e.id);
    }
    const PortCapability* out_port = nullptr;
    const PortCapability* in_port = nullptr;
    if (from != nodes.end() && from->second.cap != nullptr) {
      out_port = from->second.cap->FindOutput(e.from.port);
      if (out_port == nullptr) {
        issue(GE_STATUS_GRAPH_INVALID, "edge '" + e.id + "': node '" + e.from.node_id +
                                           "' has no output port '" + e.from.port + "'",
              e.from.node_id, e.id);
      }
    }
    if (to != nodes.end() && to->second.cap != nullptr) {
      in_port = to->second.cap->FindInput(e.to.port);
      if (in_port == nullptr) {
        issue(GE_STATUS_GRAPH_INVALID, "edge '" + e.id + "': node '" + e.to.node_id +
                                           "' has no input port '" + e.to.port + "'",
              e.to.node_id, e.id);
      }
    }
    if (out_port != nullptr) {
      OutputUse& use = outputs[e.from.ToString()];
      use.port = out_port;
      use.consumers.push_back(&e);
    }
    if (in_port != nullptr) inputs[e.to.ToString()].push_back(&e);
    if (stop()) return IssuesToStatus(issues_);
  }

  // 3. Required inputs, cardinality.
  for (const auto& [id, info] : nodes) {
    if (info.cap == nullptr) continue;
    for (const PortCapability& p : info.cap->inputs) {
      const std::string key = std::string(id) + "." + p.name;
      const auto it = inputs.find(key);
      const std::size_t count = it == inputs.end() ? 0 : it->second.size();
      if (p.required && count == 0) {
        issue(GE_STATUS_GRAPH_INVALID, "required input port '" + key + "' is not connected",
              std::string(id));
      }
      if (p.cardinality == PortCardinality::kSingle && count > 1) {
        issue(GE_STATUS_GRAPH_INVALID, "input port '" + key + "' accepts a single edge but has " +
                                           std::to_string(count), std::string(id));
      }
    }
    for (const PortCapability& p : info.cap->outputs) {
      const std::string key = std::string(id) + "." + p.name;
      const auto it = outputs.find(key);
      if (it != outputs.end() && p.cardinality == PortCardinality::kSingle &&
          it->second.consumers.size() > 1) {
        issue(GE_STATUS_GRAPH_INVALID, "output port '" + key + "' allows a single consumer but has " +
                                           std::to_string(it->second.consumers.size()),
              std::string(id));
      }
    }
  }
  if (stop()) return IssuesToStatus(issues_);

  // 4. Cycle detection on non-feedback edges (Kahn, deterministic order).
  {
    std::unordered_map<std::string_view, std::size_t> indegree;
    std::unordered_map<std::string_view, std::vector<std::string_view>> adj;
    for (const NodeSpec& n : spec.nodes()) indegree[n.id] = 0;
    for (const EdgeSpec& e : spec.edges()) {
      if (e.feedback || !nodes.contains(e.from.node_id) || !nodes.contains(e.to.node_id)) continue;
      adj[e.from.node_id].push_back(e.to.node_id);
      ++indegree[e.to.node_id];
    }
    std::vector<std::string_view> ready;
    for (const NodeSpec& n : spec.nodes()) {
      if (indegree[n.id] == 0) ready.push_back(n.id);
    }
    // Keep spec declaration order among simultaneously-ready nodes.
    const auto by_index = [&](std::string_view a, std::string_view b) {
      return nodes.at(a).index > nodes.at(b).index;
    };
    std::make_heap(ready.begin(), ready.end(), by_index);
    while (!ready.empty()) {
      std::pop_heap(ready.begin(), ready.end(), by_index);
      const std::string_view id = ready.back();
      ready.pop_back();
      out.topological_order.emplace_back(id);
      for (std::string_view next : adj[id]) {
        if (--indegree[next] == 0) {
          ready.push_back(next);
          std::push_heap(ready.begin(), ready.end(), by_index);
        }
      }
    }
    if (out.topological_order.size() != spec.nodes().size()) {
      std::vector<std::string> in_cycle;
      for (const NodeSpec& n : spec.nodes()) {
        if (indegree[n.id] != 0) in_cycle.push_back(n.id);
      }
      std::string list;
      for (const std::string& n : in_cycle) list += (list.empty() ? "" : ",") + n;
      issue(GE_STATUS_GRAPH_INVALID, "graph contains a cycle without feedback edges among [" + list + "]",
            in_cycle.empty() ? std::string{} : in_cycle.front());
    }
    for (const NodeSpec& n : spec.nodes()) {
      bool has_in = false, has_out = false;
      for (const EdgeSpec& e : spec.edges()) {
        if (e.to.node_id == n.id && !e.feedback) has_in = true;
        if (e.from.node_id == n.id) has_out = true;
      }
      if (!has_in) out.sources.push_back(n.id);
      if (!has_out) out.sinks.push_back(n.id);
    }
  }
  if (!issues_.empty()) return IssuesToStatus(issues_);

  // 5. Negotiate each used output port (single edge or fan-out).
  for (auto& [key, use] : outputs) {
    const EdgeSpec* first = use.consumers.front();
    const NodeInfo& src = nodes.at(first->from.node_id);
    Result<ConnectionContract> contract = Status::Internal("unreachable");
    if (use.consumers.size() == 1) {
      const NodeInfo& dst = nodes.at(first->to.node_id);
      const PortCapability* in_port = dst.cap->FindInput(first->to.port);
      contract = negotiator_->NegotiateEdge(src.spec->op, src.cap->Version(), *use.port,
                                            dst.spec->op, dst.cap->Version(), *in_port,
                                            first->sync, options.preferences);
    } else {
      std::vector<FanoutConsumer> consumers;
      for (const EdgeSpec* e : use.consumers) {
        const NodeInfo& dst = nodes.at(e->to.node_id);
        consumers.push_back({e->to.node_id, dst.spec->op, dst.cap->Version(),
                             dst.cap->FindInput(e->to.port), e->sync});
      }
      contract = negotiator_->NegotiateFanout(src.spec->op, src.cap->Version(), *use.port,
                                              std::move(consumers), options.preferences);
    }
    if (!contract.ok()) {
      std::string edges;
      for (const EdgeSpec* e : use.consumers) edges += (edges.empty() ? "" : ",") + e->id;
      ValidationIssue vi{contract.status().code(),
                         "output '" + key + "' [" + edges + "]: " + contract.status().message(),
                         first->from.node_id, use.consumers.size() == 1 ? first->id : std::string{}};
      if (negotiator_->last_conflict()) {
        vi.message += " conflict=" + negotiator_->last_conflict()->ToJson().Serialize();
      }
      issues_.push_back(std::move(vi));
      if (stop()) return IssuesToStatus(issues_);
      continue;
    }
    for (const EdgeSpec* e : use.consumers) out.edge_contracts[e->id] = *contract;
    out.output_contracts[key] = std::move(*contract);
  }
  if (!issues_.empty()) return IssuesToStatus(issues_);
  return out;
}

std::vector<SharedDependency> GraphValidator::FindSharedDependencies(
    const GraphSpec& spec, const std::vector<std::string>& selection,
    std::string_view entry_edge) {
  std::vector<std::string> entries;
  if (!entry_edge.empty()) entries.emplace_back(entry_edge);
  return FindSharedDependencies(spec, selection, entries);
}

std::vector<SharedDependency> GraphValidator::FindSharedDependencies(
    const GraphSpec& spec, const std::vector<std::string>& selection,
    const std::vector<std::string>& entry_edges) {
  const std::set<std::string_view> inside(selection.begin(), selection.end());
  const std::set<std::string_view> entries(entry_edges.begin(), entry_edges.end());
  std::vector<SharedDependency> deps;
  for (const EdgeSpec& e : spec.edges()) {
    if (entries.contains(e.id)) continue;
    const bool from_in = inside.contains(e.from.node_id);
    const bool to_in = inside.contains(e.to.node_id);
    if (from_in == to_in) continue;
    deps.push_back({from_in ? e.from.node_id : e.to.node_id, e.id,
                    from_in ? e.to.node_id : e.from.node_id});
  }
  return deps;
}

Result<GraphValidator::ChainBoundary> GraphValidator::CheckLinearChain(
    const GraphSpec& spec, const std::vector<std::string>& chain) {
  if (chain.empty()) return Status::GraphInvalid("chain is empty");
  std::set<std::string_view> members;
  for (const std::string& id : chain) {
    if (spec.FindNode(id) == nullptr) return Status::NotFound("chain node '" + id + "' not found");
    if (!members.insert(id).second) return Status::GraphInvalid("chain repeats node '" + id + "'");
  }
  for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
    std::size_t links = 0;
    for (const EdgeSpec& e : spec.edges()) {
      if (e.from.node_id == chain[i] && e.to.node_id == chain[i + 1]) ++links;
    }
    if (links != 1) {
      return Status::GraphInvalid("chain nodes '" + chain[i] + "' and '" + chain[i + 1] +
                                  "' must be connected by exactly one edge, found " +
                                  std::to_string(links));
    }
  }
  ChainBoundary boundary;
  JsonArray shared;
  for (const EdgeSpec& e : spec.edges()) {
    const bool from_in = members.contains(e.from.node_id);
    const bool to_in = members.contains(e.to.node_id);
    if (from_in && to_in) {
      const auto fi = std::find(chain.begin(), chain.end(), e.from.node_id) - chain.begin();
      const auto ti = std::find(chain.begin(), chain.end(), e.to.node_id) - chain.begin();
      if (ti != fi + 1) {
        return Status::GraphInvalid("edge '" + e.id + "' makes the chain non-linear");
      }
      continue;
    }
    if (!from_in && to_in) {
      if (e.to.node_id == chain.front() && boundary.inbound_edge.empty()) {
        boundary.inbound_edge = e.id;
      } else {
        shared.push_back(SharedDependency{e.to.node_id, e.id, e.from.node_id}.ToJson());
      }
    } else if (from_in && !to_in) {
      if (e.from.node_id == chain.back() && boundary.outbound_edge.empty()) {
        boundary.outbound_edge = e.id;
      } else {
        shared.push_back(SharedDependency{e.from.node_id, e.id, e.to.node_id}.ToJson());
      }
    }
  }
  if (!shared.empty()) {
    JsonObject ctx;
    ctx.emplace("dependencies", JsonValue(std::move(shared)));
    return Status::SharedDependency("chain has external dependencies beyond one input and one output",
                                    JsonValue(std::move(ctx)).Serialize());
  }
  if (boundary.inbound_edge.empty() || boundary.outbound_edge.empty()) {
    return Status::GraphInvalid("chain must have exactly one inbound and one outbound edge");
  }
  return boundary;
}

}  // namespace ge
