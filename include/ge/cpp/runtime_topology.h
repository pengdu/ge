#ifndef GE_CPP_RUNTIME_TOPOLOGY_H_
#define GE_CPP_RUNTIME_TOPOLOGY_H_

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <ge/cpp/capability.h>
#include <ge/cpp/edge_channel.h>
#include <ge/cpp/graph_spec.h>
#include <ge/cpp/graph_validator.h>
#include <ge/cpp/input_binding.h>
#include <ge/cpp/node_runtime.h>
#include <ge/cpp/operator.h>
#include <ge/cpp/types.h>

namespace ge {

struct RouteEntry {
  EdgeChannelRef edge;
  ConnectionContract contract;  // == edge->contract(); copied for lock-free reads
  TypeTagId type_tag = kInvalidTypeTag;
};

struct PortId {
  NodeId node = 0;
  std::string port;
  friend auto operator<=>(const PortId&, const PortId&) = default;
};

using RouteTable = std::map<PortId, std::vector<RouteEntry>>;
// A node's InputBinding is shared by every topology version the node lives
using InputBindingTable = std::map<NodeId, std::shared_ptr<InputBinding>>;

class RuntimeTopology final {
 public:
  struct BuildOptions {
    SessionId session_id = 0;
    TopologyVersion version = 1;
    AlignedOptions aligned;
    // Previous version (Mutation). Nodes/edges listed in |reused_nodes| /
    // |reused_edges| are carried over from it instead of re-created; their
    // input bindings are shared and rebound by ApplyRebinds(). The sets come
    // from GraphDiff (kept + updated nodes; kept edges).
    const RuntimeTopology* base = nullptr;
    std::set<std::string> reused_nodes;
    std::set<std::string> reused_edges;
    // Already validated |spec| (same factory): skip the second validation.
    const ValidatedGraph* validated = nullptr;
  };

  // Validates |spec| with |factory| capabilities, negotiates contracts,
  // creates operator instances and wires edges. Operators are *not* opened.
  [[nodiscard]] static Result<std::shared_ptr<RuntimeTopology>> Build(
      const GraphSpec& spec, OperatorFactory& factory, const BuildOptions& options);

  // by the coordinator right before the version swap; idempotent.
  void ApplyRebinds();
  // Nodes created by this version (not carried over).
  [[nodiscard]] const std::vector<NodeRuntimeRef>& new_nodes() const noexcept { return new_nodes_; }
  [[nodiscard]] const std::vector<EdgeChannelRef>& new_edges() const noexcept { return new_edges_; }

  [[nodiscard]] TopologyVersion version() const noexcept { return version_; }
  [[nodiscard]] SessionId session_id() const noexcept { return session_id_; }
  [[nodiscard]] const GraphSpec& spec() const noexcept { return spec_; }
  [[nodiscard]] const std::vector<NodeRuntimeRef>& nodes() const noexcept { return nodes_; }
  [[nodiscard]] const std::vector<EdgeChannelRef>& edges() const noexcept { return edges_; }
  [[nodiscard]] const std::vector<std::string>& topological_order() const noexcept {
    return order_;
  }
  [[nodiscard]] const ValidatedGraph& validated() const noexcept { return validated_; }

  [[nodiscard]] NodeRuntime* FindNode(NodeId id) const noexcept;
  [[nodiscard]] NodeRuntime* FindNode(std::string_view external_id) const noexcept;
  [[nodiscard]] EdgeChannel* FindEdge(std::string_view external_id) const noexcept;
  // Shared ref of an edge belonging to this topology; null when it is not
  // part of this version. O(log E) -- used by the scheduler's park path.
  [[nodiscard]] EdgeChannelRef SharedEdgeFor(const EdgeChannel* edge) const noexcept;
  [[nodiscard]] const std::vector<RouteEntry>* RoutesFor(NodeId node, std::string_view port) const noexcept;
  [[nodiscard]] InputBinding* InputsFor(NodeId node) const noexcept;
  [[nodiscard]] std::shared_ptr<InputBinding> SharedInputsFor(NodeId node) const noexcept;
  // Every output port of |node| (from its capability), for EOS fan-out.
  [[nodiscard]] std::vector<std::string> OutputPorts(NodeId node) const;

 private:
  RuntimeTopology() = default;

  SessionId session_id_ = 0;
  TopologyVersion version_ = 0;
  GraphSpec spec_;
  std::vector<NodeRuntimeRef> nodes_;
  std::vector<EdgeChannelRef> edges_;
  std::map<const EdgeChannel*, EdgeChannelRef> edge_by_ptr_;
  RouteTable routes_;
  InputBindingTable inputs_;
  std::vector<std::string> order_;
  std::map<std::string, NodeId, std::less<>> node_ids_;
  NodeId next_node_id_ = 1;
  EdgeId next_edge_id_ = 1;
  ValidatedGraph validated_;
  std::vector<NodeRuntimeRef> new_nodes_;
  std::vector<EdgeChannelRef> new_edges_;
  struct PendingRebind {
    std::vector<InputPortBinding> ports;
    SyncPolicy policy;
  };
  std::map<NodeId, PendingRebind> rebinds_;
  AlignedOptions aligned_;
};

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------

struct EmitReport {
  std::uint32_t accepted = 0;
  std::uint32_t dropped = 0;
  std::uint32_t would_block = 0;
  std::uint32_t cancelled = 0;
  std::vector<EdgeChannel*> blocked;  // edges reporting WOULD_BLOCK
  std::vector<NodeRuntime*> targets;  // downstream nodes that received data
  // The shared packet that was pushed (stamped with versions); lets a
  // caller re-push it to |blocked| edges later without duplicating it on
  // the edges that already accepted it.
  PacketRef packet;
};
[[nodiscard]] std::int64_t SteadyNowNs() noexcept;
[[nodiscard]] std::int64_t OldestIngress(const std::vector<PacketRef>& inputs) noexcept;

// Stateless: routes through the RouteTable of the topology snapshot the
class PacketRouter final {
 public:
  // Validates port ownership and type_tag against the contract, stamps
  // topology/parameter versions, then pushes to every downstream edge.
  // Returns INVALID_ARGUMENT for unknown port / type mismatch, CANCELLED if
  // the node is fast-retired, WOULD_BLOCK if any edge is backpressured
  // (other edges still received the packet), OK otherwise.
  [[nodiscard]] static Status Emit(const RuntimeTopology& topology, NodeRuntime& node,
                                   std::string_view output_port, Packet packet,
                                   ParameterVersion parameter_version,
                                   EmitReport* report = nullptr);

  // EOS to every route of |port|; retries blocked edges by reporting them
  [[nodiscard]] static Status EmitEos(const RuntimeTopology& topology, NodeRuntime& node,
                                      std::string_view output_port,
                                      ParameterVersion parameter_version,
                                      EmitReport* report = nullptr);

  // Prepare validates and stamps without pushing; PushOne pushes to a
  // single edge and accounts it in |report| exactly like Emit does.
  [[nodiscard]] static Result<PacketRef> Prepare(const RuntimeTopology& topology, NodeRuntime& node,
                                                 std::string_view output_port, Packet packet,
                                                 ParameterVersion parameter_version);
  static PushOutcome PushOne(EdgeChannel& edge, const PacketRef& packet, NodeRuntime& node,
                             EmitReport* report);
};

}  // namespace ge

#endif
