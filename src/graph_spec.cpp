#include "ge/cpp/graph_spec.h"

#include <algorithm>

namespace ge {

std::string_view ToString(ExecutorKind kind) noexcept {
  switch (kind) {
    case ExecutorKind::kAuto: return "auto";
    case ExecutorKind::kCpu: return "cpu";
    case ExecutorKind::kGpu: return "gpu";
  }
  return "auto";
}
std::string_view ToString(DropPolicy policy) noexcept {
  switch (policy) {
    case DropPolicy::kBlock: return "block";
    case DropPolicy::kDropOldest: return "drop_oldest";
    case DropPolicy::kDropNewest: return "drop_newest";
  }
  return "block";
}
std::string_view ToString(SyncPolicy policy) noexcept {
  switch (policy) {
    case SyncPolicy::kAligned: return "aligned";
    case SyncPolicy::kLatest: return "latest";
    case SyncPolicy::kAny: return "any";
  }
  return "any";
}
std::string_view ToString(RemovePolicy policy) noexcept {
  return policy == RemovePolicy::kFast ? "fast" : "drain";
}
std::string_view ToString(RemoveMode mode) noexcept {
  return mode == RemoveMode::kBypass ? "bypass" : "disconnect";
}

std::optional<ExecutorKind> ParseExecutorKind(std::string_view s) {
  if (s == "auto") return ExecutorKind::kAuto;
  if (s == "cpu") return ExecutorKind::kCpu;
  if (s == "gpu") return ExecutorKind::kGpu;
  return std::nullopt;
}
std::optional<DropPolicy> ParseDropPolicy(std::string_view s) {
  if (s == "block") return DropPolicy::kBlock;
  if (s == "drop_oldest") return DropPolicy::kDropOldest;
  if (s == "drop_newest") return DropPolicy::kDropNewest;
  return std::nullopt;
}
std::optional<SyncPolicy> ParseSyncPolicy(std::string_view s) {
  if (s == "aligned") return SyncPolicy::kAligned;
  if (s == "latest") return SyncPolicy::kLatest;
  if (s == "any") return SyncPolicy::kAny;
  return std::nullopt;
}
std::optional<RemovePolicy> ParseRemovePolicy(std::string_view s) {
  if (s == "drain") return RemovePolicy::kDrain;
  if (s == "fast") return RemovePolicy::kFast;
  return std::nullopt;
}
std::optional<RemoveMode> ParseRemoveMode(std::string_view s) {
  if (s == "disconnect") return RemoveMode::kDisconnect;
  if (s == "bypass") return RemoveMode::kBypass;
  return std::nullopt;
}

bool IsValidStableId(std::string_view id) noexcept {
  if (id.empty() || id.size() > 128) return false;
  const auto alpha = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  };
  if (!alpha(id[0])) return false;
  return std::all_of(id.begin() + 1, id.end(), [&](char c) {
    return alpha(c) || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == '-';
  });
}

std::optional<PortRef> PortRef::Parse(std::string_view text) {
  const auto dot = text.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 >= text.size()) {
    return std::nullopt;
  }
  PortRef ref{std::string(text.substr(0, dot)), std::string(text.substr(dot + 1))};
  if (!IsValidStableId(ref.node_id) || ref.port.empty()) {
    return std::nullopt;
  }
  return ref;
}

// ---------------------------------------------------------------------------

const NodeSpec* GraphSpec::FindNode(std::string_view id) const noexcept {
  for (const NodeSpec& n : nodes_) {
    if (n.id == id) return &n;
  }
  return nullptr;
}
NodeSpec* GraphSpec::FindNode(std::string_view id) noexcept {
  for (NodeSpec& n : nodes_) {
    if (n.id == id) return &n;
  }
  return nullptr;
}
const EdgeSpec* GraphSpec::FindEdge(std::string_view id) const noexcept {
  for (const EdgeSpec& e : edges_) {
    if (e.id == id) return &e;
  }
  return nullptr;
}
EdgeSpec* GraphSpec::FindEdge(std::string_view id) noexcept {
  for (EdgeSpec& e : edges_) {
    if (e.id == id) return &e;
  }
  return nullptr;
}

std::string GraphSpec::DeriveEdgeId(const PortRef& from, const PortRef& to) {
  return from.node_id + "." + from.port + "->" + to.node_id + "." + to.port;
}

Status GraphSpec::AddNode(NodeSpec node) {
  if (!IsValidStableId(node.id)) {
    return Status::GraphInvalid("invalid node id '" + node.id + "'");
  }
  if (FindNode(node.id) != nullptr) {
    return Status::AlreadyExists("duplicate node id '" + node.id + "'");
  }
  if (node.op.type_name.empty()) {
    return Status::GraphInvalid("node '" + node.id + "' has empty operator");
  }
  if (node.parallelism == 0) {
    return Status::GraphInvalid("node '" + node.id + "' parallelism must be >= 1");
  }
  if (node.executor.kind != ExecutorKind::kGpu && node.executor.device_id) {
    return Status::GraphInvalid("node '" + node.id +
                                "' device_id only allowed for gpu executor");
  }
  nodes_.push_back(std::move(node));
  return Status::Ok();
}

Status GraphSpec::AddEdge(EdgeSpec edge) {
  if (edge.id.empty()) {
    edge.id = DeriveEdgeId(edge.from, edge.to);
  }
  if (!IsValidStableId(edge.id) &&
      edge.id != DeriveEdgeId(edge.from, edge.to)) {
    return Status::GraphInvalid("invalid edge id '" + edge.id + "'");
  }
  if (FindEdge(edge.id) != nullptr) {
    return Status::AlreadyExists("duplicate edge id '" + edge.id + "'");
  }
  if (FindNode(edge.from.node_id) == nullptr) {
    return Status::GraphInvalid("edge '" + edge.id + "' from unknown node '" +
                                edge.from.node_id + "'");
  }
  if (FindNode(edge.to.node_id) == nullptr) {
    return Status::GraphInvalid("edge '" + edge.id + "' to unknown node '" +
                                edge.to.node_id + "'");
  }
  if (edge.from.port.empty() || edge.to.port.empty()) {
    return Status::GraphInvalid("edge '" + edge.id + "' has empty port");
  }
  if (edge.queue.capacity == 0) {
    return Status::GraphInvalid("edge '" + edge.id + "' capacity must be >= 1");
  }
  if (edge.queue.max_packet_bytes && *edge.queue.max_packet_bytes == 0) {
    return Status::GraphInvalid("edge '" + edge.id +
                                "' max_packet_bytes must be > 0");
  }
  for (const EdgeSpec& e : edges_) {
    if (e.from == edge.from && e.to == edge.to) {
      return Status::AlreadyExists("edge " + edge.from.ToString() + "->" +
                                   edge.to.ToString() + " already exists");
    }
  }
  edges_.push_back(std::move(edge));
  return Status::Ok();
}

Status GraphSpec::RemoveNode(std::string_view id) {
  const auto it = std::find_if(nodes_.begin(), nodes_.end(),
                               [&](const NodeSpec& n) { return n.id == id; });
  if (it == nodes_.end()) {
    return Status::NotFound("node '" + std::string(id) + "' not found");
  }
  nodes_.erase(it);
  std::erase_if(edges_, [&](const EdgeSpec& e) {
    return e.from.node_id == id || e.to.node_id == id;
  });
  return Status::Ok();
}

Status GraphSpec::RemoveEdge(std::string_view id) {
  const auto it = std::find_if(edges_.begin(), edges_.end(),
                               [&](const EdgeSpec& e) { return e.id == id; });
  if (it == edges_.end()) {
    return Status::NotFound("edge '" + std::string(id) + "' not found");
  }
  edges_.erase(it);
  return Status::Ok();
}

std::vector<const EdgeSpec*> GraphSpec::EdgesFrom(std::string_view node_id) const {
  std::vector<const EdgeSpec*> out;
  for (const EdgeSpec& e : edges_) {
    if (e.from.node_id == node_id) out.push_back(&e);
  }
  return out;
}
std::vector<const EdgeSpec*> GraphSpec::EdgesTo(std::string_view node_id) const {
  std::vector<const EdgeSpec*> out;
  for (const EdgeSpec& e : edges_) {
    if (e.to.node_id == node_id) out.push_back(&e);
  }
  return out;
}
std::vector<const EdgeSpec*> GraphSpec::EdgesFromPort(const PortRef& port) const {
  std::vector<const EdgeSpec*> out;
  for (const EdgeSpec& e : edges_) {
    if (e.from == port) out.push_back(&e);
  }
  return out;
}
std::vector<const EdgeSpec*> GraphSpec::EdgesToPort(const PortRef& port) const {
  std::vector<const EdgeSpec*> out;
  for (const EdgeSpec& e : edges_) {
    if (e.to == port) out.push_back(&e);
  }
  return out;
}

// ---------------------------------------------------------------------------

std::string_view ActionTypeName(const MutationAction& action) noexcept {
  return std::visit(
      [](const auto& a) -> std::string_view {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, AddNodeAction>) return "add_node";
        else if constexpr (std::is_same_v<T, RemoveNodeAction>) return "remove_node";
        else if constexpr (std::is_same_v<T, InsertChainAction>) return "insert_chain";
        else if constexpr (std::is_same_v<T, RemoveChainAction>) return "remove_chain";
        else if constexpr (std::is_same_v<T, ReplaceNodeAction>) return "replace_node";
        else if constexpr (std::is_same_v<T, RemoveBranchAction>) return "remove_branch";
        else if constexpr (std::is_same_v<T, AddEdgeAction>) return "add_edge";
        else if constexpr (std::is_same_v<T, RemoveEdgeAction>) return "remove_edge";
        else return "set_node_options";
      },
      action);
}

// ---------------------------------------------------------------------------

NodeRef GraphBuilder::AddNode(OperatorKey key, std::string id, JsonValue options) {
  NodeSpec node;
  node.id = id;
  node.op = std::move(key);
  node.options = std::move(options);
  return AddNode(std::move(node));
}

NodeRef GraphBuilder::AddNode(NodeSpec node) {
  NodeRef ref{node.id};
  if (Status s = spec_.AddNode(std::move(node)); !s.ok()) {
    errors_.push_back(std::move(s));
  }
  return ref;
}

GraphBuilder& GraphBuilder::Connect(PortRef from, PortRef to, EdgeOptions options) {
  EdgeSpec edge;
  edge.id = options.id.value_or("");
  edge.from = std::move(from);
  edge.to = std::move(to);
  edge.queue = options.queue;
  edge.sync = options.sync;
  edge.feedback = options.feedback;
  if (Status s = spec_.AddEdge(std::move(edge)); !s.ok()) {
    errors_.push_back(std::move(s));
  }
  return *this;
}

GraphBuilder& GraphBuilder::SetDescription(std::string description) {
  spec_.set_description(std::move(description));
  return *this;
}

GraphBuilder& GraphBuilder::SetOptions(GraphOptions options) {
  spec_.mutable_options() = std::move(options);
  return *this;
}

Result<GraphSpec> GraphBuilder::Build() const {
  if (!errors_.empty()) {
    return errors_.front();
  }
  if (spec_.nodes().empty()) {
    return Status::GraphInvalid("graph '" + spec_.name() + "' has no nodes");
  }
  return spec_;
}

// ---------------------------------------------------------------------------

Mutation& Mutation::SetBaseVersion(TopologyVersion version) {
  patch_.base_topology_version = version;
  return *this;
}
Mutation& Mutation::SetRemovePolicy(RemovePolicy policy) {
  patch_.remove_policy = policy;
  return *this;
}
Mutation& Mutation::AddNode(NodeSpec node) {
  patch_.actions.emplace_back(AddNodeAction{std::move(node)});
  return *this;
}
Mutation& Mutation::InsertChain(std::string edge, std::vector<NodeSpec> chain,
                                ChainPorts ports) {
  patch_.actions.emplace_back(
      InsertChainAction{std::move(edge), std::move(chain), std::move(ports)});
  return *this;
}
Mutation& Mutation::RemoveNode(std::string node, RemoveOptions options) {
  patch_.actions.emplace_back(RemoveNodeAction{
      std::move(node), RemoveMode::kDisconnect, options.remove_policy});
  return *this;
}
Mutation& Mutation::RemoveChain(std::vector<std::string> chain,
                                RemoveChainOptions options) {
  patch_.actions.emplace_back(RemoveChainAction{
      std::move(chain),
      options.bypass ? RemoveMode::kBypass : RemoveMode::kDisconnect,
      options.remove_policy});
  return *this;
}
Mutation& Mutation::ReplaceNode(std::string old_node, OperatorKey replacement,
                                JsonValue options,
                                std::optional<PortMapping> mapping,
                                RemoveOptions remove) {
  patch_.actions.emplace_back(ReplaceNodeAction{
      std::move(old_node), std::move(replacement), std::move(options),
      std::move(mapping), remove.remove_policy});
  return *this;
}
Mutation& Mutation::RemoveBranch(std::string entry_edge,
                                 BranchSelection selection,
                                 RemoveOptions options) {
  patch_.actions.emplace_back(RemoveBranchAction{
      std::move(entry_edge), std::move(selection.nodes), options.remove_policy, {}});
  return *this;
}
Mutation& Mutation::RemoveBranch(std::vector<std::string> entry_edges,
                                 BranchSelection selection,
                                 RemoveOptions options) {
  RemoveBranchAction a;
  if (!entry_edges.empty()) a.entry_edge = entry_edges.front();
  a.nodes = std::move(selection.nodes);
  a.remove_policy = options.remove_policy;
  a.entry_edges = std::move(entry_edges);
  patch_.actions.emplace_back(std::move(a));
  return *this;
}
Mutation& Mutation::AddEdge(PortRef from, PortRef to, EdgeOptions options) {
  EdgeSpec edge;
  edge.id = options.id.value_or("");
  edge.from = std::move(from);
  edge.to = std::move(to);
  edge.queue = options.queue;
  edge.sync = options.sync;
  edge.feedback = options.feedback;
  patch_.actions.emplace_back(AddEdgeAction{std::move(edge)});
  return *this;
}
Mutation& Mutation::RemoveEdge(std::string edge) {
  patch_.actions.emplace_back(RemoveEdgeAction{std::move(edge)});
  return *this;
}
Mutation& Mutation::SetNodeOptions(std::string node, JsonValue parameters) {
  patch_.actions.emplace_back(
      SetNodeOptionsAction{std::move(node), std::move(parameters)});
  return *this;
}

}  // namespace ge
