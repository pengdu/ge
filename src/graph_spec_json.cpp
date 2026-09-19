#include "ge/cpp/graph_spec_json.h"

#include "json_reader.h"

namespace ge {
namespace {

using internal::ObjectReader;
using internal::CheckDocumentHeader;
using internal::MarkHeaderSeen;

Status ParseOperatorKey(const std::string& text, std::string_view path,
                        OperatorKey* out) {
  auto key = OperatorKey::Parse(text);
  if (!key) {
    return Status::GraphInvalid("invalid operator '" + text + "' in " +
                                std::string(path) + " (expected type@version)");
  }
  *out = std::move(*key);
  return Status::Ok();
}

Status ParseNode(const JsonValue& value, const std::string& path,
                 bool allow_unknown, NodeSpec* out) {
  ObjectReader r(value, path, allow_unknown);
  if (!r.ok()) return Status::GraphInvalid(path + " must be an object");
  if (Status s = r.RequireString("id", &out->id); !s.ok()) return s;
  std::string op;
  if (Status s = r.RequireString("operator", &op); !s.ok()) return s;
  if (Status s = ParseOperatorKey(op, path, &out->op); !s.ok()) return s;

  if (const JsonValue* ex = r.Get("executor"); ex != nullptr) {
    ObjectReader er(*ex, path + ".executor", allow_unknown);
    if (!er.ok()) return Status::GraphInvalid(path + ".executor must be an object");
    std::string kind = "auto";
    if (Status s = er.OptionalString("kind", &kind); !s.ok()) return s;
    auto parsed = ParseExecutorKind(kind);
    if (!parsed) return Status::GraphInvalid("invalid executor.kind '" + kind + "' in " + path);
    out->executor.kind = *parsed;
    std::optional<std::int64_t> device;
    if (Status s = er.OptionalInteger("device_id", &device); !s.ok()) return s;
    if (device) {
      if (out->executor.kind != ExecutorKind::kGpu) {
        return Status::GraphInvalid(path + ": device_id only allowed when executor.kind is gpu");
      }
      out->executor.device_id = static_cast<std::int32_t>(*device);
    }
    if (Status s = er.Finish(); !s.ok()) return s;
  }

  std::optional<std::int64_t> parallelism;
  if (Status s = r.OptionalInteger("parallelism", &parallelism); !s.ok()) return s;
  if (parallelism) {
    if (*parallelism < 1) return Status::GraphInvalid(path + ": parallelism must be >= 1");
    out->parallelism = static_cast<std::uint32_t>(*parallelism);
  }
  if (Status s = r.OptionalObject("options", &out->options); !s.ok()) return s;

  if (const JsonValue* labels = r.Get("labels"); labels != nullptr) {
    if (!labels->is_object()) return r.WrongType("labels", "object");
    for (const auto& [k, v] : labels->as_object()) {
      if (!v.is_string()) {
        return Status::GraphInvalid(path + ".labels." + k + " must be a string");
      }
      out->labels[k] = v.as_string();
    }
  }
  return r.Finish();
}

Status ParseEndpoint(ObjectReader& r, std::string_view key, PortRef* out) {
  std::string text;
  if (Status s = r.RequireString(key, &text); !s.ok()) return s;
  auto ref = PortRef::Parse(text);
  if (!ref) {
    return Status::GraphInvalid("invalid endpoint '" + text + "' in " + r.path() +
                                " (expected <node_id>.<port>)");
  }
  *out = std::move(*ref);
  return Status::Ok();
}

Status ParseEdge(const JsonValue& value, const std::string& path,
                 bool allow_unknown, EdgeSpec* out) {
  ObjectReader r(value, path, allow_unknown);
  if (!r.ok()) return Status::GraphInvalid(path + " must be an object");
  if (Status s = r.OptionalString("id", &out->id); !s.ok()) return s;
  if (Status s = ParseEndpoint(r, "from", &out->from); !s.ok()) return s;
  if (Status s = ParseEndpoint(r, "to", &out->to); !s.ok()) return s;

  if (const JsonValue* q = r.Get("queue"); q != nullptr) {
    ObjectReader qr(*q, path + ".queue", allow_unknown);
    if (!qr.ok()) return Status::GraphInvalid(path + ".queue must be an object");
    std::optional<std::int64_t> capacity;
    if (Status s = qr.OptionalInteger("capacity", &capacity); !s.ok()) return s;
    if (capacity) {
      if (*capacity < 1) return Status::GraphInvalid(path + ": queue.capacity must be >= 1");
      out->queue.capacity = static_cast<std::uint32_t>(*capacity);
    }
    std::string policy;
    if (Status s = qr.OptionalString("policy", &policy); !s.ok()) return s;
    if (!policy.empty()) {
      auto p = ParseDropPolicy(policy);
      if (!p) return Status::GraphInvalid("invalid queue.policy '" + policy + "' in " + path);
      out->queue.policy = *p;
    }
    std::optional<std::int64_t> max_bytes;
    if (Status s = qr.OptionalInteger("max_packet_bytes", &max_bytes); !s.ok()) return s;
    if (max_bytes) {
      if (*max_bytes <= 0) return Status::GraphInvalid(path + ": queue.max_packet_bytes must be > 0");
      out->queue.max_packet_bytes = static_cast<std::uint64_t>(*max_bytes);
    }
    if (Status s = qr.Finish(); !s.ok()) return s;
  }

  std::string sync;
  if (Status s = r.OptionalString("sync", &sync); !s.ok()) return s;
  if (!sync.empty()) {
    auto p = ParseSyncPolicy(sync);
    if (!p) return Status::GraphInvalid("invalid sync '" + sync + "' in " + path);
    out->sync = *p;
  }
  if (Status s = r.OptionalBool("feedback", &out->feedback); !s.ok()) return s;
  return r.Finish();
}

Status ParseRemovePolicyField(ObjectReader& r, std::optional<RemovePolicy>* out) {
  std::string text;
  if (Status s = r.OptionalString("remove_policy", &text); !s.ok()) return s;
  if (text.empty()) return Status::Ok();
  auto p = ParseRemovePolicy(text);
  if (!p) return Status::GraphInvalid("invalid remove_policy '" + text + "' in " + r.path());
  *out = *p;
  return Status::Ok();
}

Status ParseModeField(ObjectReader& r, RemoveMode* out) {
  std::string text;
  if (Status s = r.OptionalString("mode", &text); !s.ok()) return s;
  if (text.empty()) return Status::Ok();
  auto m = ParseRemoveMode(text);
  if (!m) return Status::GraphInvalid("invalid mode '" + text + "' in " + r.path());
  *out = *m;
  return Status::Ok();
}

Status ParseAction(const JsonValue& value, const std::string& path,
                   bool allow_unknown, MutationAction* out) {
  ObjectReader r(value, path, allow_unknown);
  if (!r.ok()) return Status::GraphInvalid(path + " must be an object");
  std::string type;
  if (Status s = r.RequireString("type", &type); !s.ok()) return s;

  if (type == "add_node") {
    AddNodeAction a;
    const JsonValue* node = r.Get("node");
    if (node == nullptr) return r.Missing("node");
    if (Status s = ParseNode(*node, path + ".node", allow_unknown, &a.node); !s.ok()) return s;
    *out = std::move(a);
  } else if (type == "remove_node") {
    RemoveNodeAction a;
    if (Status s = r.RequireString("node", &a.node); !s.ok()) return s;
    if (Status s = ParseModeField(r, &a.mode); !s.ok()) return s;
    if (Status s = ParseRemovePolicyField(r, &a.remove_policy); !s.ok()) return s;
    *out = std::move(a);
  } else if (type == "insert_chain") {
    InsertChainAction a;
    if (Status s = r.RequireString("edge", &a.edge); !s.ok()) return s;
    const JsonValue* nodes = r.Get("nodes");
    if (nodes == nullptr) return r.Missing("nodes");
    if (!nodes->is_array() || nodes->as_array().empty()) {
      return Status::GraphInvalid(path + ".nodes must be a non-empty array");
    }
    std::size_t i = 0;
    for (const JsonValue& n : nodes->as_array()) {
      NodeSpec node;
      if (Status s = ParseNode(n, path + ".nodes[" + std::to_string(i++) + "]",
                               allow_unknown, &node);
          !s.ok()) return s;
      a.nodes.push_back(std::move(node));
    }
    if (const JsonValue* ports = r.Get("ports"); ports != nullptr) {
      ObjectReader pr(*ports, path + ".ports", allow_unknown);
      if (!pr.ok()) return Status::GraphInvalid(path + ".ports must be an object");
      std::string in, outp;
      if (Status s = pr.OptionalString("chain_input", &in); !s.ok()) return s;
      if (Status s = pr.OptionalString("chain_output", &outp); !s.ok()) return s;
      if (!in.empty()) a.ports.chain_input = in;
      if (!outp.empty()) a.ports.chain_output = outp;
      if (Status s = pr.Finish(); !s.ok()) return s;
    }
    *out = std::move(a);
  } else if (type == "remove_chain") {
    RemoveChainAction a;
    if (Status s = r.StringArray("nodes", true, &a.nodes); !s.ok()) return s;
    if (a.nodes.empty()) return Status::GraphInvalid(path + ".nodes must be non-empty");
    if (Status s = ParseModeField(r, &a.mode); !s.ok()) return s;
    if (Status s = ParseRemovePolicyField(r, &a.remove_policy); !s.ok()) return s;
    *out = std::move(a);
  } else if (type == "replace_node") {
    ReplaceNodeAction a;
    if (Status s = r.RequireString("node", &a.node); !s.ok()) return s;
    const JsonValue* rep = r.Get("replacement");
    if (rep == nullptr) return r.Missing("replacement");
    ObjectReader rr(*rep, path + ".replacement", allow_unknown);
    if (!rr.ok()) return Status::GraphInvalid(path + ".replacement must be an object");
    std::string op;
    if (Status s = rr.RequireString("operator", &op); !s.ok()) return s;
    if (Status s = ParseOperatorKey(op, rr.path(), &a.replacement_op); !s.ok()) return s;
    if (Status s = rr.OptionalObject("options", &a.replacement_options); !s.ok()) return s;
    if (Status s = rr.Finish(); !s.ok()) return s;
    if (const JsonValue* pm = r.Get("port_mapping"); pm != nullptr) {
      ObjectReader mr(*pm, path + ".port_mapping", allow_unknown);
      if (!mr.ok()) return Status::GraphInvalid(path + ".port_mapping must be an object");
      PortMapping mapping;
      for (const char* dir : {"inputs", "outputs"}) {
        const JsonValue* m = mr.Get(dir);
        if (m == nullptr) continue;
        if (!m->is_object()) return mr.WrongType(dir, "object");
        auto& target = std::string_view(dir) == "inputs" ? mapping.inputs : mapping.outputs;
        for (const auto& [k, v] : m->as_object()) {
          if (!v.is_string()) return Status::GraphInvalid(path + ".port_mapping." + dir + "." + k + " must be a string");
          target[k] = v.as_string();
        }
      }
      if (Status s = mr.Finish(); !s.ok()) return s;
      a.port_mapping = std::move(mapping);
    }
    if (Status s = ParseRemovePolicyField(r, &a.remove_policy); !s.ok()) return s;
    *out = std::move(a);
  } else if (type == "remove_branch") {
    RemoveBranchAction a;
    if (Status s = r.OptionalString("entry_edge", &a.entry_edge); !s.ok()) return s;
    if (Status s = r.StringArray("entry_edges", false, &a.entry_edges); !s.ok()) return s;
    if (a.entry_edge.empty() && a.entry_edges.empty()) {
      return Status::GraphInvalid(path + " needs entry_edge or a non-empty entry_edges");
    }
    if (a.entry_edge.empty()) a.entry_edge = a.entry_edges.front();
    if (Status s = r.StringArray("nodes", true, &a.nodes); !s.ok()) return s;
    if (a.nodes.empty()) return Status::GraphInvalid(path + ".nodes must be non-empty");
    if (Status s = ParseRemovePolicyField(r, &a.remove_policy); !s.ok()) return s;
    *out = std::move(a);
  } else if (type == "add_edge") {
    AddEdgeAction a;
    const JsonValue* edge = r.Get("edge");
    if (edge == nullptr) return r.Missing("edge");
    if (Status s = ParseEdge(*edge, path + ".edge", allow_unknown, &a.edge); !s.ok()) return s;
    *out = std::move(a);
  } else if (type == "remove_edge") {
    RemoveEdgeAction a;
    if (Status s = r.RequireString("edge", &a.edge); !s.ok()) return s;
    *out = std::move(a);
  } else if (type == "set_node_options") {
    SetNodeOptionsAction a;
    if (Status s = r.RequireString("node", &a.node); !s.ok()) return s;
    const JsonValue* params = r.Get("parameters");
    if (params == nullptr) return r.Missing("parameters");
    if (!params->is_object()) return r.WrongType("parameters", "object");
    a.parameters = *params;
    *out = std::move(a);
  } else {
    return Status::GraphInvalid("unknown action type '" + type + "' in " + path);
  }
  return r.Finish();
}

JsonValue EnumJson(std::string_view s) { return JsonValue(std::string(s)); }

}  // namespace

// ---------------------------------------------------------------------------

Result<GraphSpec> GraphSpecParser::ParseGraph(std::string_view json) {
  JsonParseResult parsed = ParseJson(json);
  if (!parsed.ok()) return Status::GraphInvalid("invalid JSON: " + parsed.error);
  return ParseGraph(*parsed.value);
}

Result<GraphSpec> GraphSpecParser::ParseGraph(const JsonValue& doc) {
  bool allow_unknown = false;
  if (Status s = CheckDocumentHeader(doc, "GraphSpec", GraphSpec::kSchemaVersion,
                             GraphSpec::kSchemaId, &allow_unknown);
      !s.ok()) return s;

  ObjectReader r(doc, "GraphSpec", allow_unknown);
  MarkHeaderSeen(r);

  std::string name;
  if (Status s = r.RequireString("name", &name); !s.ok()) return s;
  GraphSpec spec(std::move(name));
  spec.set_allow_unknown_fields(allow_unknown);
  std::string description;
  if (Status s = r.OptionalString("description", &description); !s.ok()) return s;
  spec.set_description(std::move(description));

  if (const JsonValue* opts = r.Get("options"); opts != nullptr) {
    if (!opts->is_object()) return r.WrongType("options", "object");
    GraphOptions& go = spec.mutable_options();
    JsonObject extra;
    for (const auto& [k, v] : opts->as_object()) {
      if (k == "default_device_id") {
        if (!v.is_integer()) return Status::GraphInvalid("options.default_device_id must be integer");
        go.default_device_id = static_cast<std::int32_t>(v.as_integer());
      } else if (k == "allow_default_operator_version") {
        if (!v.is_bool()) return Status::GraphInvalid("options.allow_default_operator_version must be boolean");
        go.allow_default_operator_version = v.as_bool();
      } else {
        extra.emplace(k, v);
      }
    }
    go.extra = JsonValue(std::move(extra));
  }

  const JsonValue* nodes = r.Get("nodes");
  if (nodes == nullptr) return r.Missing("nodes");
  if (!nodes->is_array()) return r.WrongType("nodes", "array");
  if (nodes->as_array().empty()) return Status::GraphInvalid("GraphSpec.nodes must contain at least one node");
  std::size_t i = 0;
  for (const JsonValue& n : nodes->as_array()) {
    NodeSpec node;
    if (Status s = ParseNode(n, "nodes[" + std::to_string(i++) + "]", allow_unknown, &node); !s.ok()) return s;
    if (Status s = spec.AddNode(std::move(node)); !s.ok()) return s;
  }

  const JsonValue* edges = r.Get("edges");
  if (edges == nullptr) return r.Missing("edges");
  if (!edges->is_array()) return r.WrongType("edges", "array");
  i = 0;
  for (const JsonValue& e : edges->as_array()) {
    EdgeSpec edge;
    if (Status s = ParseEdge(e, "edges[" + std::to_string(i++) + "]", allow_unknown, &edge); !s.ok()) return s;
    if (Status s = spec.AddEdge(std::move(edge)); !s.ok()) return s;
  }

  if (Status s = r.Finish(); !s.ok()) return s;
  return spec;
}

JsonValue GraphSpecParser::ToJson(const NodeSpec& node) {
  JsonObject o;
  o.emplace("id", JsonValue(node.id));
  o.emplace("operator", JsonValue(node.op.ToString()));
  if (node.executor.kind != ExecutorKind::kAuto || node.executor.device_id) {
    JsonObject ex;
    ex.emplace("kind", EnumJson(ToString(node.executor.kind)));
    if (node.executor.device_id) {
      ex.emplace("device_id", JsonValue(static_cast<std::int64_t>(*node.executor.device_id)));
    }
    o.emplace("executor", JsonValue(std::move(ex)));
  }
  if (node.parallelism != 1) {
    o.emplace("parallelism", JsonValue(static_cast<std::int64_t>(node.parallelism)));
  }
  if (!node.options.as_object().empty()) o.emplace("options", node.options);
  if (!node.labels.empty()) {
    JsonObject labels;
    for (const auto& [k, v] : node.labels) labels.emplace(k, JsonValue(v));
    o.emplace("labels", JsonValue(std::move(labels)));
  }
  return JsonValue(std::move(o));
}

JsonValue GraphSpecParser::ToJson(const EdgeSpec& edge) {
  JsonObject o;
  if (!edge.id.empty()) o.emplace("id", JsonValue(edge.id));
  o.emplace("from", JsonValue(edge.from.ToString()));
  o.emplace("to", JsonValue(edge.to.ToString()));
  if (edge.queue != QueueSpec{}) {
    JsonObject q;
    q.emplace("capacity", JsonValue(static_cast<std::int64_t>(edge.queue.capacity)));
    q.emplace("policy", EnumJson(ToString(edge.queue.policy)));
    if (edge.queue.max_packet_bytes) {
      q.emplace("max_packet_bytes", JsonValue(static_cast<std::int64_t>(*edge.queue.max_packet_bytes)));
    }
    o.emplace("queue", JsonValue(std::move(q)));
  }
  if (edge.sync) o.emplace("sync", EnumJson(ToString(*edge.sync)));
  if (edge.feedback) o.emplace("feedback", JsonValue(true));
  return JsonValue(std::move(o));
}

JsonValue GraphSpecParser::ToJson(const GraphSpec& spec) {
  JsonObject o;
  o.emplace("kind", JsonValue("GraphSpec"));
  o.emplace("schema_version", JsonValue(static_cast<std::int64_t>(GraphSpec::kSchemaVersion)));
  o.emplace("$id", JsonValue(GraphSpec::kSchemaId));
  if (spec.allow_unknown_fields()) o.emplace("allow_unknown_fields", JsonValue(true));
  o.emplace("name", JsonValue(spec.name()));
  if (!spec.description().empty()) o.emplace("description", JsonValue(spec.description()));

  JsonObject opts = spec.options().extra.as_object();
  if (spec.options().default_device_id) {
    opts.emplace("default_device_id", JsonValue(static_cast<std::int64_t>(*spec.options().default_device_id)));
  }
  if (spec.options().allow_default_operator_version) {
    opts.emplace("allow_default_operator_version", JsonValue(true));
  }
  if (!opts.empty()) o.emplace("options", JsonValue(std::move(opts)));

  JsonArray nodes;
  for (const NodeSpec& n : spec.nodes()) nodes.push_back(ToJson(n));
  o.emplace("nodes", JsonValue(std::move(nodes)));
  JsonArray edges;
  for (const EdgeSpec& e : spec.edges()) edges.push_back(ToJson(e));
  o.emplace("edges", JsonValue(std::move(edges)));
  return JsonValue(std::move(o));
}

std::string GraphSpecParser::SerializeGraph(const GraphSpec& spec) {
  return ToJson(spec).Serialize();
}

// ---------------------------------------------------------------------------

Result<MutationPatch> GraphSpecParser::ParsePatch(std::string_view json) {
  JsonParseResult parsed = ParseJson(json);
  if (!parsed.ok()) return Status::GraphInvalid("invalid JSON: " + parsed.error);
  return ParsePatch(*parsed.value);
}

Result<MutationPatch> GraphSpecParser::ParsePatch(const JsonValue& doc) {
  bool allow_unknown = false;
  if (Status s = CheckDocumentHeader(doc, "MutationPatch", MutationPatch::kSchemaVersion,
                             MutationPatch::kSchemaId, &allow_unknown);
      !s.ok()) return s;

  ObjectReader r(doc, "MutationPatch", allow_unknown);
  MarkHeaderSeen(r);

  MutationPatch patch;
  patch.allow_unknown_fields = allow_unknown;
  std::optional<std::int64_t> base;
  if (Status s = r.OptionalInteger("base_topology_version", &base); !s.ok()) return s;
  if (base) {
    if (*base < 0) return Status::GraphInvalid("base_topology_version must be >= 0");
    patch.base_topology_version = static_cast<TopologyVersion>(*base);
  }
  std::optional<RemovePolicy> policy;
  if (Status s = ParseRemovePolicyField(r, &policy); !s.ok()) return s;
  if (policy) patch.remove_policy = *policy;

  const JsonValue* actions = r.Get("actions");
  if (actions == nullptr) return r.Missing("actions");
  if (!actions->is_array()) return r.WrongType("actions", "array");
  std::size_t i = 0;
  for (const JsonValue& a : actions->as_array()) {
    MutationAction action;
    if (Status s = ParseAction(a, "actions[" + std::to_string(i++) + "]", allow_unknown, &action); !s.ok()) return s;
    patch.actions.push_back(std::move(action));
  }
  if (Status s = r.Finish(); !s.ok()) return s;
  return patch;
}

JsonValue GraphSpecParser::ToJson(const MutationPatch& patch) {
  JsonObject o;
  o.emplace("kind", JsonValue("MutationPatch"));
  o.emplace("schema_version", JsonValue(static_cast<std::int64_t>(MutationPatch::kSchemaVersion)));
  o.emplace("$id", JsonValue(MutationPatch::kSchemaId));
  if (patch.allow_unknown_fields) o.emplace("allow_unknown_fields", JsonValue(true));
  if (patch.base_topology_version) {
    o.emplace("base_topology_version", JsonValue(static_cast<std::int64_t>(*patch.base_topology_version)));
  }
  o.emplace("remove_policy", EnumJson(ToString(patch.remove_policy)));

  const auto policy_json = [](const std::optional<RemovePolicy>& p, JsonObject& obj) {
    if (p) obj.emplace("remove_policy", EnumJson(ToString(*p)));
  };
  const auto string_array = [](const std::vector<std::string>& v) {
    JsonArray a;
    for (const std::string& s : v) a.push_back(JsonValue(s));
    return JsonValue(std::move(a));
  };

  JsonArray actions;
  for (const MutationAction& action : patch.actions) {
    JsonObject a;
    a.emplace("type", EnumJson(ActionTypeName(action)));
    std::visit(
        [&](const auto& act) {
          using T = std::decay_t<decltype(act)>;
          if constexpr (std::is_same_v<T, AddNodeAction>) {
            a.emplace("node", ToJson(act.node));
          } else if constexpr (std::is_same_v<T, RemoveNodeAction>) {
            a.emplace("node", JsonValue(act.node));
            a.emplace("mode", EnumJson(ToString(act.mode)));
            policy_json(act.remove_policy, a);
          } else if constexpr (std::is_same_v<T, InsertChainAction>) {
            a.emplace("edge", JsonValue(act.edge));
            JsonArray nodes;
            for (const NodeSpec& n : act.nodes) nodes.push_back(ToJson(n));
            a.emplace("nodes", JsonValue(std::move(nodes)));
            if (act.ports.chain_input || act.ports.chain_output) {
              JsonObject ports;
              if (act.ports.chain_input) ports.emplace("chain_input", JsonValue(*act.ports.chain_input));
              if (act.ports.chain_output) ports.emplace("chain_output", JsonValue(*act.ports.chain_output));
              a.emplace("ports", JsonValue(std::move(ports)));
            }
          } else if constexpr (std::is_same_v<T, RemoveChainAction>) {
            a.emplace("nodes", string_array(act.nodes));
            a.emplace("mode", EnumJson(ToString(act.mode)));
            policy_json(act.remove_policy, a);
          } else if constexpr (std::is_same_v<T, ReplaceNodeAction>) {
            a.emplace("node", JsonValue(act.node));
            JsonObject rep;
            rep.emplace("operator", JsonValue(act.replacement_op.ToString()));
            if (!act.replacement_options.as_object().empty()) rep.emplace("options", act.replacement_options);
            a.emplace("replacement", JsonValue(std::move(rep)));
            if (act.port_mapping) {
              JsonObject pm;
              JsonObject in, out;
              for (const auto& [k, v] : act.port_mapping->inputs) in.emplace(k, JsonValue(v));
              for (const auto& [k, v] : act.port_mapping->outputs) out.emplace(k, JsonValue(v));
              pm.emplace("inputs", JsonValue(std::move(in)));
              pm.emplace("outputs", JsonValue(std::move(out)));
              a.emplace("port_mapping", JsonValue(std::move(pm)));
            }
            policy_json(act.remove_policy, a);
          } else if constexpr (std::is_same_v<T, RemoveBranchAction>) {
            if (act.entry_edges.empty()) {
              a.emplace("entry_edge", JsonValue(act.entry_edge));
            } else {
              a.emplace("entry_edges", string_array(act.entry_edges));
            }
            a.emplace("nodes", string_array(act.nodes));
            policy_json(act.remove_policy, a);
          } else if constexpr (std::is_same_v<T, AddEdgeAction>) {
            a.emplace("edge", ToJson(act.edge));
          } else if constexpr (std::is_same_v<T, RemoveEdgeAction>) {
            a.emplace("edge", JsonValue(act.edge));
          } else {
            a.emplace("node", JsonValue(act.node));
            a.emplace("parameters", act.parameters);
          }
        },
        action);
    actions.push_back(JsonValue(std::move(a)));
  }
  o.emplace("actions", JsonValue(std::move(actions)));
  return JsonValue(std::move(o));
}

std::string GraphSpecParser::SerializePatch(const MutationPatch& patch) {
  return ToJson(patch).Serialize();
}

}  // namespace ge
