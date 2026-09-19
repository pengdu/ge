#ifndef GE_CPP_GRAPH_SPEC_H_
#define GE_CPP_GRAPH_SPEC_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge {

// ---------------------------------------------------------------------------
// GraphSpec: the JSON intermediate representation of a graph
// (14-JSON-Schema设计 §2). External node/edge ids are stable strings; the
// engine maps them to NodeId/EdgeId internally and never writes the mapping
// back into the spec.
// ---------------------------------------------------------------------------

enum class ExecutorKind : std::uint8_t { kAuto, kCpu, kGpu };
enum class DropPolicy : std::uint8_t { kBlock, kDropOldest, kDropNewest };
enum class SyncPolicy : std::uint8_t { kAligned, kLatest, kAny };
enum class RemovePolicy : std::uint8_t { kDrain, kFast };
enum class RemoveMode : std::uint8_t { kDisconnect, kBypass };

[[nodiscard]] std::string_view ToString(ExecutorKind kind) noexcept;
[[nodiscard]] std::string_view ToString(DropPolicy policy) noexcept;
[[nodiscard]] std::string_view ToString(SyncPolicy policy) noexcept;
[[nodiscard]] std::string_view ToString(RemovePolicy policy) noexcept;
[[nodiscard]] std::string_view ToString(RemoveMode mode) noexcept;
[[nodiscard]] std::optional<ExecutorKind> ParseExecutorKind(std::string_view);
[[nodiscard]] std::optional<DropPolicy> ParseDropPolicy(std::string_view);
[[nodiscard]] std::optional<SyncPolicy> ParseSyncPolicy(std::string_view);
[[nodiscard]] std::optional<RemovePolicy> ParseRemovePolicy(std::string_view);
[[nodiscard]] std::optional<RemoveMode> ParseRemoveMode(std::string_view);

// Stable external id: ^[A-Za-z][A-Za-z0-9_.-]{0,127}$
[[nodiscard]] bool IsValidStableId(std::string_view id) noexcept;

struct ExecutorSpec {
  ExecutorKind kind = ExecutorKind::kAuto;
  std::optional<std::int32_t> device_id;
  friend bool operator==(const ExecutorSpec&, const ExecutorSpec&) = default;
};

struct NodeSpec {
  std::string id;
  OperatorKey op;
  ExecutorSpec executor;
  std::uint32_t parallelism = 1;
  JsonValue options = JsonValue(JsonObject{});
  std::map<std::string, std::string> labels;
  friend bool operator==(const NodeSpec&, const NodeSpec&) = default;
};

// "<node_id>.<port>"
struct PortRef {
  std::string node_id;
  std::string port;

  [[nodiscard]] std::string ToString() const { return node_id + "." + port; }
  [[nodiscard]] static std::optional<PortRef> Parse(std::string_view text);
  friend bool operator==(const PortRef&, const PortRef&) = default;
  friend auto operator<=>(const PortRef&, const PortRef&) = default;
};

struct QueueSpec {
  std::uint32_t capacity = 64;
  DropPolicy policy = DropPolicy::kBlock;
  std::optional<std::uint64_t> max_packet_bytes;
  friend bool operator==(const QueueSpec&, const QueueSpec&) = default;
};

struct EdgeSpec {
  std::string id;  // empty => derived by GraphSpec::AddEdge
  PortRef from;
  PortRef to;
  QueueSpec queue;
  std::optional<SyncPolicy> sync;  // nullopt => target port's default
  bool feedback = false;
  friend bool operator==(const EdgeSpec&, const EdgeSpec&) = default;
};

struct GraphOptions {
  std::optional<std::int32_t> default_device_id;
  bool allow_default_operator_version = false;
  JsonValue extra = JsonValue(JsonObject{});  // unrecognised keys, kept for export
  friend bool operator==(const GraphOptions&, const GraphOptions&) = default;
};

class GraphSpec final {
 public:
  static constexpr int kSchemaVersion = 1;
  static constexpr std::string_view kSchemaId = "ge.dev/schema/graph/v1";

  GraphSpec() = default;
  explicit GraphSpec(std::string name) : name_(std::move(name)) {}

  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  void set_name(std::string name) { name_ = std::move(name); }
  [[nodiscard]] const std::string& description() const noexcept {
    return description_;
  }
  void set_description(std::string d) { description_ = std::move(d); }
  [[nodiscard]] const GraphOptions& options() const noexcept { return options_; }
  [[nodiscard]] GraphOptions& mutable_options() noexcept { return options_; }
  [[nodiscard]] bool allow_unknown_fields() const noexcept {
    return allow_unknown_fields_;
  }
  void set_allow_unknown_fields(bool v) noexcept { allow_unknown_fields_ = v; }

  [[nodiscard]] const std::vector<NodeSpec>& nodes() const noexcept {
    return nodes_;
  }
  [[nodiscard]] const std::vector<EdgeSpec>& edges() const noexcept {
    return edges_;
  }

  [[nodiscard]] const NodeSpec* FindNode(std::string_view id) const noexcept;
  [[nodiscard]] NodeSpec* FindNode(std::string_view id) noexcept;
  [[nodiscard]] const EdgeSpec* FindEdge(std::string_view id) const noexcept;
  [[nodiscard]] EdgeSpec* FindEdge(std::string_view id) noexcept;

  // Validates ids and uniqueness only; topology/ports are the validator's job.
  [[nodiscard]] Status AddNode(NodeSpec node);
  // Derives an id when EdgeSpec::id is empty. Endpoint nodes must exist.
  [[nodiscard]] Status AddEdge(EdgeSpec edge);
  // Removes the node and every edge touching it.
  [[nodiscard]] Status RemoveNode(std::string_view id);
  [[nodiscard]] Status RemoveEdge(std::string_view id);

  [[nodiscard]] std::vector<const EdgeSpec*> EdgesFrom(
      std::string_view node_id) const;
  [[nodiscard]] std::vector<const EdgeSpec*> EdgesTo(
      std::string_view node_id) const;
  [[nodiscard]] std::vector<const EdgeSpec*> EdgesFromPort(
      const PortRef& port) const;
  [[nodiscard]] std::vector<const EdgeSpec*> EdgesToPort(const PortRef& port) const;

  [[nodiscard]] static std::string DeriveEdgeId(const PortRef& from,
                                                const PortRef& to);

  friend bool operator==(const GraphSpec&, const GraphSpec&) = default;

 private:
  std::string name_;
  std::string description_;
  GraphOptions options_;
  bool allow_unknown_fields_ = false;
  std::vector<NodeSpec> nodes_;
  std::vector<EdgeSpec> edges_;
};

// ---------------------------------------------------------------------------
// MutationPatch (14 §3). Actions are applied in order to a candidate GraphSpec
// by the control plane; this header only carries the data.
// ---------------------------------------------------------------------------

struct ChainPorts {
  std::optional<std::string> chain_input;
  std::optional<std::string> chain_output;
  friend bool operator==(const ChainPorts&, const ChainPorts&) = default;
};

struct PortMapping {
  std::map<std::string, std::string> inputs;   // old port -> new port
  std::map<std::string, std::string> outputs;
  friend bool operator==(const PortMapping&, const PortMapping&) = default;
};

struct AddNodeAction {
  NodeSpec node;
  friend bool operator==(const AddNodeAction&, const AddNodeAction&) = default;
};
struct RemoveNodeAction {
  std::string node;
  RemoveMode mode = RemoveMode::kDisconnect;
  std::optional<RemovePolicy> remove_policy;
  friend bool operator==(const RemoveNodeAction&, const RemoveNodeAction&) = default;
};
struct InsertChainAction {
  std::string edge;
  std::vector<NodeSpec> nodes;
  ChainPorts ports;
  friend bool operator==(const InsertChainAction&, const InsertChainAction&) = default;
};
struct RemoveChainAction {
  std::vector<std::string> nodes;
  RemoveMode mode = RemoveMode::kDisconnect;
  std::optional<RemovePolicy> remove_policy;
  friend bool operator==(const RemoveChainAction&, const RemoveChainAction&) = default;
};
struct ReplaceNodeAction {
  std::string node;
  OperatorKey replacement_op;
  JsonValue replacement_options = JsonValue(JsonObject{});
  std::optional<PortMapping> port_mapping;
  std::optional<RemovePolicy> remove_policy;
  friend bool operator==(const ReplaceNodeAction&, const ReplaceNodeAction&) = default;
};
struct RemoveBranchAction {
  std::string entry_edge;
  std::vector<std::string> nodes;
  std::optional<RemovePolicy> remove_policy;
  // P6: a branch may hang off several upstream ports (video + audio). When
  // non-empty this list is authoritative and |entry_edge| is ignored.
  std::vector<std::string> entry_edges;
  friend bool operator==(const RemoveBranchAction&, const RemoveBranchAction&) = default;
  [[nodiscard]] std::vector<std::string> EntryEdges() const {
    return entry_edges.empty() ? std::vector<std::string>{entry_edge} : entry_edges;
  }
};
struct AddEdgeAction {
  EdgeSpec edge;
  friend bool operator==(const AddEdgeAction&, const AddEdgeAction&) = default;
};
struct RemoveEdgeAction {
  std::string edge;
  friend bool operator==(const RemoveEdgeAction&, const RemoveEdgeAction&) = default;
};
struct SetNodeOptionsAction {
  std::string node;
  JsonValue parameters = JsonValue(JsonObject{});
  friend bool operator==(const SetNodeOptionsAction&, const SetNodeOptionsAction&) = default;
};

using MutationAction =
    std::variant<AddNodeAction, RemoveNodeAction, InsertChainAction,
                 RemoveChainAction, ReplaceNodeAction, RemoveBranchAction,
                 AddEdgeAction, RemoveEdgeAction, SetNodeOptionsAction>;

[[nodiscard]] std::string_view ActionTypeName(const MutationAction& action) noexcept;

struct MutationPatch {
  static constexpr int kSchemaVersion = 1;
  static constexpr std::string_view kSchemaId = "ge.dev/schema/patch/v1";

  std::optional<TopologyVersion> base_topology_version;
  RemovePolicy remove_policy = RemovePolicy::kDrain;
  std::vector<MutationAction> actions;
  bool allow_unknown_fields = false;
  friend bool operator==(const MutationPatch&, const MutationPatch&) = default;
};

// ---------------------------------------------------------------------------
// Builders (13 §6.2). Pure data transformation; no validation beyond ids.
// ---------------------------------------------------------------------------

struct EdgeOptions {
  std::optional<std::string> id;
  QueueSpec queue;
  std::optional<SyncPolicy> sync;
  bool feedback = false;
};

struct NodeRef {
  std::string id;
  [[nodiscard]] PortRef port(std::string_view name) const {
    return PortRef{id, std::string(name)};
  }
};

class GraphBuilder final {
 public:
  explicit GraphBuilder(std::string name) : spec_(std::move(name)) {}

  NodeRef AddNode(OperatorKey key, std::string id,
                  JsonValue options = JsonValue(JsonObject{}));
  NodeRef AddNode(NodeSpec node);
  GraphBuilder& Connect(PortRef from, PortRef to, EdgeOptions options = {});
  GraphBuilder& SetDescription(std::string description);
  GraphBuilder& SetOptions(GraphOptions options);

  [[nodiscard]] Result<GraphSpec> Build() const;

 private:
  GraphSpec spec_;
  std::vector<Status> errors_;
};

struct RemoveOptions {
  std::optional<RemovePolicy> remove_policy;
};
struct RemoveChainOptions {
  bool bypass = false;
  std::optional<RemovePolicy> remove_policy;
};
struct BranchSelection {
  std::vector<std::string> nodes;
};

class Mutation final {
 public:
  Mutation() = default;

  Mutation& SetBaseVersion(TopologyVersion version);
  Mutation& SetRemovePolicy(RemovePolicy policy);
  Mutation& AddNode(NodeSpec node);
  Mutation& InsertChain(std::string edge, std::vector<NodeSpec> chain,
                        ChainPorts ports = {});
  Mutation& RemoveNode(std::string node, RemoveOptions options = {});
  Mutation& RemoveChain(std::vector<std::string> chain,
                        RemoveChainOptions options = {});
  Mutation& ReplaceNode(std::string old_node, OperatorKey replacement,
                        JsonValue options = JsonValue(JsonObject{}),
                        std::optional<PortMapping> mapping = std::nullopt,
                        RemoveOptions remove = {});
  Mutation& RemoveBranch(std::string entry_edge, BranchSelection selection,
                         RemoveOptions options = {});
  // P6: multi-entry branch (12 §7.6).
  Mutation& RemoveBranch(std::vector<std::string> entry_edges, BranchSelection selection,
                         RemoveOptions options = {});
  Mutation& AddEdge(PortRef from, PortRef to, EdgeOptions options = {});
  Mutation& RemoveEdge(std::string edge);
  Mutation& SetNodeOptions(std::string node, JsonValue parameters);

  [[nodiscard]] const MutationPatch& patch() const noexcept { return patch_; }
  [[nodiscard]] MutationPatch Build() const { return patch_; }

 private:
  MutationPatch patch_;
};

}  // namespace ge

#endif
