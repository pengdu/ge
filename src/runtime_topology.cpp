#include <ge/cpp/runtime_topology.h>

#include <algorithm>
#include <chrono>
#include <optional>

namespace ge {

namespace {

// Ports of |node| in capability declaration order; unconnected optional
// ports still appear (never ready).
std::vector<InputPortBinding> OrderedPorts(const NodeRuntime& node,
                                           std::vector<InputPortBinding> ports) {
  for (const PortCapability& p : node.capability().inputs) {
    const bool bound = std::any_of(ports.begin(), ports.end(),
                                   [&](const InputPortBinding& b) { return b.port == p.name; });
    if (!bound) ports.push_back({p.name, nullptr, p.required, {}});
  }
  const auto idx = [&](const std::string& name) {
    const auto& ins = node.capability().inputs;
    return std::find_if(ins.begin(), ins.end(), [&](const auto& p) { return p.name == name; }) -
           ins.begin();
  };
  std::stable_sort(ports.begin(), ports.end(),
                   [&](const auto& a, const auto& b) { return idx(a.port) < idx(b.port); });
  return ports;
}

SyncPolicy PolicyOf(const std::vector<InputPortBinding>& ports) {
  for (const InputPortBinding& b : ports) {
    if (b.edge) return b.edge->contract().sync_policy;
  }
  return SyncPolicy::kAny;
}

}  // namespace

Result<std::shared_ptr<RuntimeTopology>> RuntimeTopology::Build(const GraphSpec& spec,
                                                                OperatorFactory& factory,
                                                                const BuildOptions& options) {
  std::optional<ValidatedGraph> own;
  const ValidatedGraph* validated = options.validated;
  if (validated == nullptr) {
    GraphValidator validator([&factory](const OperatorKey& k) { return factory.Describe(k); });
    auto r = validator.Validate(spec);
    if (!r.ok()) return r.status();
    own = std::move(*r);
    validated = &*own;
  }

  std::shared_ptr<RuntimeTopology> t(new RuntimeTopology);
  t->session_id_ = options.session_id;
  t->version_ = options.version;
  t->spec_ = spec;
  t->order_ = validated->topological_order;
  t->validated_ = *validated;
  t->aligned_ = options.aligned;
  if (options.base != nullptr) {
    t->next_node_id_ = options.base->next_node_id_;
    t->next_edge_id_ = options.base->next_edge_id_;
  }

  // Nodes.
  for (const NodeSpec& n : spec.nodes()) {
    if (options.base != nullptr && options.reused_nodes.contains(n.id)) {
      NodeRuntime* base_node = options.base->FindNode(n.id);
      if (base_node == nullptr) return Status::Internal("reused node '" + n.id + "' missing in base");
      NodeRuntimeRef ref;
      for (const NodeRuntimeRef& r : options.base->nodes_) {
        if (r.get() == base_node) ref = r;
      }
      t->nodes_.push_back(ref);
      t->node_ids_[n.id] = ref->id();
      continue;
    }
    const CapabilityDescriptor* cap = factory.Describe(n.op);
    if (cap == nullptr) return Status::NotFound("operator '" + n.op.ToString() + "' not found");
    OperatorCreateArgs args{n.op, t->next_node_id_, n.id, n.options, options.session_id,
                            options.version};
    auto op = factory.Create(args);
    if (!op.ok()) return op.status();
    auto node = std::make_shared<NodeRuntime>(t->next_node_id_, n.id, n.op, *cap,
                                              std::move(*op), n.options, n.parallelism);
    t->node_ids_[n.id] = t->next_node_id_;
    ++t->next_node_id_;
    t->new_nodes_.push_back(node);
    t->nodes_.push_back(std::move(node));
  }

  // Edges + routes.
  std::map<NodeId, std::vector<InputPortBinding>> bindings;
  for (const EdgeSpec& e : spec.edges()) {
    const ConnectionContract& contract = validated->edge_contracts.at(e.id);
    EdgeChannelRef edge;
    if (options.base != nullptr && options.reused_edges.contains(e.id)) {
      for (const EdgeChannelRef& r : options.base->edges_) {
        if (r->external_id() == e.id) edge = r;
      }
      if (!edge) return Status::Internal("reused edge '" + e.id + "' missing in base");
      // The reuse decision is GraphDiff's (contract/endpoint/queue changes
      // classify the edge as recreated, so it is not in |reused_edges|).
      if (!edge->contract().SameDataPlane(contract)) {
        return Status::Internal("reused edge '" + e.id + "' has a different data contract");
      }
    }
    const NodeId from = t->node_ids_.at(e.from.node_id);
    const NodeId to = t->node_ids_.at(e.to.node_id);
    if (!edge) {
      EdgeConfig cfg;
      cfg.capacity = e.queue.capacity;
      cfg.policy = e.queue.policy;
      cfg.sync_policy = contract.sync_policy;
      edge = std::make_shared<EdgeChannel>(t->next_edge_id_++, e.id, contract, cfg, e.from, e.to);
      NodeRuntimeRef p, c;
      for (const NodeRuntimeRef& r : t->nodes_) {
        if (r->id() == from) p = r;
        if (r->id() == to) c = r;
      }
      edge->Bind(p, c);  // reused edges keep their (identical) binding
      t->new_edges_.push_back(edge);
    }
    t->edges_.push_back(edge);
    t->routes_[PortId{from, e.from.port}].push_back(
        {edge, contract, TypeTagRegistry::Global().Intern(contract.logical_type)});
    const NodeRuntime* target = t->FindNode(to);
    const PortCapability* in_port = target->capability().FindInput(e.to.port);
    bindings[to].push_back({e.to.port, edge, in_port != nullptr && in_port->required, {}});
  }

  // Input bindings. Reused nodes share the base binding; if their port table
  // changed the rebind is deferred to publish (ApplyRebinds).
  for (const NodeRuntimeRef& node : t->nodes_) {
    std::vector<InputPortBinding> ports;
    if (auto it = bindings.find(node->id()); it != bindings.end()) ports = std::move(it->second);
    ports = OrderedPorts(*node, std::move(ports));
    const SyncPolicy policy = PolicyOf(ports);
    const bool reused = options.base != nullptr && options.reused_nodes.contains(node->external_id());
    if (reused) {
      std::shared_ptr<InputBinding> shared = options.base->SharedInputsFor(node->id());
      if (!shared) return Status::Internal("reused node '" + node->external_id() + "' has no binding");
      t->inputs_[node->id()] = shared;
      bool same = true;
      const auto current = shared->ports();
      if (current.size() != ports.size() || shared->policy() != policy) {
        same = false;
      } else {
        for (std::size_t i = 0; i < ports.size(); ++i) {
          if (current[i].port != ports[i].port || current[i].edge != ports[i].edge ||
              current[i].required != ports[i].required) {
            same = false;
          }
        }
      }
      if (!same) t->rebinds_[node->id()] = PendingRebind{std::move(ports), policy};
      continue;
    }
    t->inputs_[node->id()] = std::make_shared<InputBinding>(std::move(ports), policy, options.aligned);
  }
  return t;
}

void RuntimeTopology::ApplyRebinds() {
  for (auto& [id, pending] : rebinds_) {
    if (const auto it = inputs_.find(id); it != inputs_.end()) {
      it->second->Rebind(pending.ports, pending.policy, aligned_);
    }
  }
  rebinds_.clear();
}

NodeRuntime* RuntimeTopology::FindNode(NodeId id) const noexcept {
  for (const NodeRuntimeRef& n : nodes_) {
    if (n->id() == id) return n.get();
  }
  return nullptr;
}

NodeRuntime* RuntimeTopology::FindNode(std::string_view external_id) const noexcept {
  const auto it = node_ids_.find(external_id);
  return it == node_ids_.end() ? nullptr : FindNode(it->second);
}

EdgeChannel* RuntimeTopology::FindEdge(std::string_view external_id) const noexcept {
  for (const EdgeChannelRef& e : edges_) {
    if (e->external_id() == external_id) return e.get();
  }
  return nullptr;
}

const std::vector<RouteEntry>* RuntimeTopology::RoutesFor(NodeId node,
                                                          std::string_view port) const noexcept {
  const auto it = routes_.find(PortId{node, std::string(port)});
  return it == routes_.end() ? nullptr : &it->second;
}

InputBinding* RuntimeTopology::InputsFor(NodeId node) const noexcept {
  const auto it = inputs_.find(node);
  return it == inputs_.end() ? nullptr : it->second.get();
}

std::shared_ptr<InputBinding> RuntimeTopology::SharedInputsFor(NodeId node) const noexcept {
  const auto it = inputs_.find(node);
  return it == inputs_.end() ? nullptr : it->second;
}

std::vector<std::string> RuntimeTopology::OutputPorts(NodeId node) const {
  std::vector<std::string> out;
  if (const NodeRuntime* n = FindNode(node)) {
    for (const PortCapability& p : n->capability().outputs) out.push_back(p.name);
  }
  return out;
}

// ---------------------------------------------------------------------------
// PacketRouter
// ---------------------------------------------------------------------------

std::int64_t SteadyNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::int64_t OldestIngress(const std::vector<PacketRef>& inputs) noexcept {
  std::int64_t oldest = 0;
  for (const PacketRef& p : inputs) {
    if (!p || p->ingress_ns == 0) continue;
    if (oldest == 0 || p->ingress_ns < oldest) oldest = p->ingress_ns;
  }
  return oldest;
}

namespace {

PushOutcome PushEdge(EdgeChannel& edge, const PacketRef& packet, EmitReport* report,
                     NodeRuntime& node) {
  const bool control = packet && (packet->is_eos() || packet->is_event());
  const PushOutcome o = edge.Push(packet);
  switch (o) {
    case PushOutcome::kAccepted:
    case PushOutcome::kAcceptedDroppedOldest:
      if (!control) node.metrics().packets_out.fetch_add(1, std::memory_order_relaxed);
      if (report != nullptr) {
        ++report->accepted;
        if (o == PushOutcome::kAcceptedDroppedOldest) ++report->dropped;
        if (const std::shared_ptr<NodeRuntime> t = edge.consumer()) {
          report->targets.push_back(t.get());
        }
      }
      break;
    case PushOutcome::kDroppedNewest:
      if (report != nullptr) ++report->dropped;
      break;
    case PushOutcome::kWouldBlock:
      node.metrics().would_block.fetch_add(1, std::memory_order_relaxed);
      if (report != nullptr) {
        ++report->would_block;
        report->blocked.push_back(&edge);
      }
      break;
    case PushOutcome::kCancelled:
      if (report != nullptr) ++report->cancelled;
      break;
  }
  return o;
}

Status PushAll(const std::vector<RouteEntry>& routes, PacketRef packet, EmitReport* report,
               NodeRuntime& node) {
  bool blocked = false;
  for (const RouteEntry& r : routes) {
    if (PushEdge(*r.edge, packet, report, node) == PushOutcome::kWouldBlock) blocked = true;
  }
  return blocked ? Status::WouldBlock() : Status::Ok();
}

}  // namespace

Result<PacketRef> PacketRouter::Prepare(const RuntimeTopology& topology, NodeRuntime& node,
                                        std::string_view output_port, Packet packet,
                                        ParameterVersion parameter_version) {
  if (node.cancelled()) return Status::Cancelled("node '" + node.external_id() + "' is retired");
  if (node.capability().FindOutput(output_port) == nullptr) {
    return Status::InvalidArgument("node '" + node.external_id() + "' has no output port '" +
                                   std::string(output_port) + "'");
  }
  const std::vector<RouteEntry>* routes = topology.RoutesFor(node.id(), output_port);
  if (routes == nullptr || routes->empty()) return PacketRef{};  // unconnected output
  const RouteEntry& first = routes->front();
  if (!packet.is_eos() && !packet.is_event() && !packet.is_dropped() &&
      packet.header.type_tag != first.type_tag) {
    return Status::InvalidArgument(
        "packet type '" + std::string(TypeTagRegistry::Global().Name(packet.header.type_tag)) +
        "' does not match contract '" + first.contract.logical_type + "' on " +
        node.external_id() + "." + std::string(output_port));
  }
  packet.header.topology_version = topology.version();
  packet.header.parameter_version = parameter_version;
  if (packet.header.type_tag == kInvalidTypeTag) packet.header.type_tag = first.type_tag;
  // OBS-1 end-to-end: a source stamps "now", everything else inherits the
  // oldest input of the invocation that produced this packet.
  if (packet.ingress_ns == 0 && !packet.is_eos() && !packet.is_event()) {
    if (node.is_source()) {
      packet.ingress_ns = SteadyNowNs();
    } else {
      packet.ingress_ns = node.metrics().current_ingress_ns.load(std::memory_order_relaxed);
    }
  }
  return std::make_shared<const Packet>(std::move(packet));
}

PushOutcome PacketRouter::PushOne(EdgeChannel& edge, const PacketRef& packet, NodeRuntime& node,
                                  EmitReport* report) {
  return PushEdge(edge, packet, report, node);
}

Status PacketRouter::Emit(const RuntimeTopology& topology, NodeRuntime& node,
                          std::string_view output_port, Packet packet,
                          ParameterVersion parameter_version, EmitReport* report) {
  Result<PacketRef> shared = Prepare(topology, node, output_port, std::move(packet), parameter_version);
  if (!shared.ok()) return shared.status();
  if (!*shared) return Status::Ok();  // unconnected output
  const std::vector<RouteEntry>* routes = topology.RoutesFor(node.id(), output_port);
  if (report != nullptr) report->packet = *shared;
  return PushAll(*routes, std::move(*shared), report, node);
}

Status PacketRouter::EmitEos(const RuntimeTopology& topology, NodeRuntime& node,
                             std::string_view output_port, ParameterVersion parameter_version,
                             EmitReport* report) {
  const std::vector<RouteEntry>* routes = topology.RoutesFor(node.id(), output_port);
  if (routes == nullptr || routes->empty()) return Status::Ok();
  Packet eos = Packet::Eos(routes->front().type_tag, topology.version(), parameter_version,
                           node.last_seq());
  return PushAll(*routes, std::make_shared<const Packet>(std::move(eos)), report, node);
}

}  // namespace ge
