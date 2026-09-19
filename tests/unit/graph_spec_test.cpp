#include <ge/cpp/graph_spec.h>
#include <ge/cpp/graph_spec_json.h>

#include <gtest/gtest.h>

namespace {

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

TEST(GraphSpecTest, StableIdRules) {
  EXPECT_TRUE(ge::IsValidStableId("a"));
  EXPECT_TRUE(ge::IsValidStableId("decoder_1.main-x"));
  EXPECT_FALSE(ge::IsValidStableId(""));
  EXPECT_FALSE(ge::IsValidStableId("1abc"));
  EXPECT_FALSE(ge::IsValidStableId("a b"));
  EXPECT_FALSE(ge::IsValidStableId(std::string(129, 'a')));
  EXPECT_TRUE(ge::IsValidStableId(std::string(128, 'a')));
}

TEST(GraphSpecTest, PortRefParse) {
  const auto ref = ge::PortRef::Parse("decoder.video");
  ASSERT_TRUE(ref.has_value());
  EXPECT_EQ(ref->node_id, "decoder");
  EXPECT_EQ(ref->port, "video");
  EXPECT_FALSE(ge::PortRef::Parse("decoder").has_value());
  EXPECT_FALSE(ge::PortRef::Parse(".x").has_value());
  EXPECT_FALSE(ge::PortRef::Parse("a.").has_value());
}

TEST(GraphSpecTest, AddNodeRejectsInvalid) {
  ge::GraphSpec spec("g");
  EXPECT_TRUE(spec.AddNode({.id = "a", .op = Op("X@1.0.0")}).ok());
  EXPECT_EQ(spec.AddNode({.id = "a", .op = Op("X@1.0.0")}).code(), GE_STATUS_ALREADY_EXISTS);
  EXPECT_EQ(spec.AddNode({.id = "9", .op = Op("X@1.0.0")}).code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(spec.AddNode({.id = "b", .op = Op("X@1.0.0"), .parallelism = 0}).code(), GE_STATUS_GRAPH_INVALID);
  ge::NodeSpec cpu_with_device{.id = "c", .op = Op("X@1.0.0")};
  cpu_with_device.executor.kind = ge::ExecutorKind::kCpu;
  cpu_with_device.executor.device_id = 0;
  EXPECT_EQ(spec.AddNode(cpu_with_device).code(), GE_STATUS_GRAPH_INVALID);
}

TEST(GraphSpecTest, AddEdgeDerivesIdAndChecksEndpoints) {
  ge::GraphSpec spec("g");
  ASSERT_TRUE(spec.AddNode({.id = "a", .op = Op("X@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "b", .op = Op("Y@1.0.0")}).ok());
  ge::EdgeSpec e{.from = {"a", "out"}, .to = {"b", "in"}};
  ASSERT_TRUE(spec.AddEdge(e).ok());
  EXPECT_EQ(spec.edges()[0].id, "a.out->b.in");
  EXPECT_EQ(spec.AddEdge(e).code(), GE_STATUS_ALREADY_EXISTS);
  ge::EdgeSpec dup_pair{.id = "e2", .from = {"a", "out"}, .to = {"b", "in"}};
  EXPECT_EQ(spec.AddEdge(dup_pair).code(), GE_STATUS_ALREADY_EXISTS);
  ge::EdgeSpec second_in{.id = "e3", .from = {"a", "out2"}, .to = {"b", "in"}};
  EXPECT_TRUE(spec.AddEdge(second_in).ok());
  EXPECT_EQ(spec.EdgesToPort({"b", "in"}).size(), 2U);
  ge::EdgeSpec unknown{.from = {"zz", "out"}, .to = {"b", "in2"}};
  EXPECT_EQ(spec.AddEdge(unknown).code(), GE_STATUS_GRAPH_INVALID);
}

TEST(GraphSpecTest, RemoveNodeDropsIncidentEdges) {
  ge::GraphSpec spec("g");
  ASSERT_TRUE(spec.AddNode({.id = "a", .op = Op("X@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "b", .op = Op("Y@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "c", .op = Op("Z@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddEdge({.from = {"a", "o"}, .to = {"b", "i"}}).ok());
  ASSERT_TRUE(spec.AddEdge({.from = {"b", "o"}, .to = {"c", "i"}}).ok());
  ASSERT_TRUE(spec.RemoveNode("b").ok());
  EXPECT_EQ(spec.nodes().size(), 2U);
  EXPECT_TRUE(spec.edges().empty());
  EXPECT_EQ(spec.RemoveNode("b").code(), GE_STATUS_NOT_FOUND);
}

TEST(GraphBuilderTest, BuildsSpec) {
  ge::GraphBuilder b("live");
  const auto src = b.AddNode(Op("Src@1.0.0"), "src");
  const auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  b.Connect(src.port("out"), sink.port("in"), {.id = "e1"});
  auto spec = b.Build();
  ASSERT_TRUE(spec.ok()) << spec.status().ToString();
  EXPECT_EQ(spec->nodes().size(), 2U);
  EXPECT_EQ(spec->edges()[0].id, "e1");
}

TEST(GraphBuilderTest, ReportsFirstError) {
  ge::GraphBuilder b("bad");
  b.AddNode(Op("Src@1.0.0"), "src");
  b.Connect({"src", "out"}, {"missing", "in"});
  EXPECT_EQ(b.Build().status().code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(ge::GraphBuilder("empty").Build().status().code(), GE_STATUS_GRAPH_INVALID);
}

TEST(MutationBuilderTest, RecordsActionsInOrder) {
  ge::Mutation m;
  m.SetBaseVersion(7)
      .SetRemovePolicy(ge::RemovePolicy::kFast)
      .AddNode({.id = "n", .op = Op("Op@1.0.0")})
      .RemoveChain({"a", "b"}, {.bypass = true})
      .SetNodeOptions("enc", ge::JsonValue(ge::JsonObject{{"bitrate", ge::JsonValue(2500000)}}));
  const ge::MutationPatch& p = m.patch();
  EXPECT_EQ(p.base_topology_version, 7U);
  EXPECT_EQ(p.remove_policy, ge::RemovePolicy::kFast);
  ASSERT_EQ(p.actions.size(), 3U);
  EXPECT_EQ(ge::ActionTypeName(p.actions[0]), "add_node");
  EXPECT_EQ(ge::ActionTypeName(p.actions[1]), "remove_chain");
  EXPECT_EQ(std::get<ge::RemoveChainAction>(p.actions[1]).mode, ge::RemoveMode::kBypass);
  EXPECT_EQ(ge::ActionTypeName(p.actions[2]), "set_node_options");
}

// ---------------------------------------------------------------------------

constexpr const char* kGraphJson = R"({
  "kind": "GraphSpec",
  "schema_version": 1,
  "$id": "ge.dev/schema/graph/v1",
  "name": "live_transcode",
  "description": "demo",
  "options": {"default_device_id": 0, "allow_default_operator_version": false, "custom": "x"},
  "nodes": [
    {"id": "decoder", "operator": "VideoDecoder@2.0.0",
     "executor": {"kind": "gpu", "device_id": 0}, "parallelism": 1,
     "options": {"hardware": "cuda"}, "labels": {"role": "public-prefix"}},
    {"id": "scale", "operator": "VideoScale@1.0.0", "parallelism": 2},
    {"id": "sink", "operator": "Sink@1.0.0"}
  ],
  "edges": [
    {"id": "decoder-to-scale", "from": "decoder.video", "to": "scale.in",
     "queue": {"capacity": 64, "policy": "drop_oldest", "max_packet_bytes": 8388608},
     "sync": "any", "feedback": false},
    {"from": "scale.out", "to": "sink.in"}
  ]
})";

TEST(GraphSpecJsonTest, ParsesFullDocument) {
  const auto r = ge::GraphSpecParser::ParseGraph(kGraphJson);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  const ge::GraphSpec& spec = *r;
  EXPECT_EQ(spec.name(), "live_transcode");
  EXPECT_EQ(spec.description(), "demo");
  EXPECT_EQ(spec.options().default_device_id, 0);
  EXPECT_EQ(spec.options().extra.GetString("custom"), "x");
  ASSERT_EQ(spec.nodes().size(), 3U);
  const ge::NodeSpec* dec = spec.FindNode("decoder");
  ASSERT_NE(dec, nullptr);
  EXPECT_EQ(dec->op.ToString(), "VideoDecoder@2.0.0");
  EXPECT_EQ(dec->executor.kind, ge::ExecutorKind::kGpu);
  EXPECT_EQ(dec->executor.device_id, 0);
  EXPECT_EQ(dec->options.GetString("hardware"), "cuda");
  EXPECT_EQ(dec->labels.at("role"), "public-prefix");
  EXPECT_EQ(spec.FindNode("scale")->parallelism, 2U);
  ASSERT_EQ(spec.edges().size(), 2U);
  const ge::EdgeSpec* e = spec.FindEdge("decoder-to-scale");
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(e->queue.capacity, 64U);
  EXPECT_EQ(e->queue.policy, ge::DropPolicy::kDropOldest);
  EXPECT_EQ(e->queue.max_packet_bytes, 8388608U);
  EXPECT_EQ(e->sync, ge::SyncPolicy::kAny);
  EXPECT_EQ(spec.edges()[1].id, "scale.out->sink.in");
  EXPECT_FALSE(spec.edges()[1].sync.has_value());
}

TEST(GraphSpecJsonTest, RoundTripIsStable) {
  const auto r = ge::GraphSpecParser::ParseGraph(kGraphJson);
  ASSERT_TRUE(r.ok());
  const std::string once = ge::GraphSpecParser::SerializeGraph(*r);
  const auto again = ge::GraphSpecParser::ParseGraph(once);
  ASSERT_TRUE(again.ok()) << again.status().ToString();
  EXPECT_EQ(*r, *again);
  EXPECT_EQ(once, ge::GraphSpecParser::SerializeGraph(*again));
}

TEST(GraphSpecJsonTest, HeaderRules) {
  EXPECT_EQ(ge::GraphSpecParser::ParseGraph(R"({"schema_version":1,"name":"g","nodes":[{"id":"a","operator":"X@1"}],"edges":[]})").status().code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(ge::GraphSpecParser::ParseGraph(R"({"kind":"GraphSpec","schema_version":2,"name":"g","nodes":[],"edges":[]})").status().code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(ge::GraphSpecParser::ParseGraph(R"({"kind":"GraphSpec","schema_version":1,"$id":"ge.dev/schema/graph/v2","name":"g","nodes":[],"edges":[]})").status().code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(ge::GraphSpecParser::ParseGraph(R"({"kind":"MutationPatch","schema_version":1,"name":"g","nodes":[],"edges":[]})").status().code(), GE_STATUS_GRAPH_INVALID);
}

TEST(GraphSpecJsonTest, UnknownFieldsRejectedUnlessAllowed) {
  const char* strict = R"({"kind":"GraphSpec","schema_version":1,"name":"g",
    "nodes":[{"id":"a","operator":"X@1.0.0","typo":1}],"edges":[]})";
  const auto r1 = ge::GraphSpecParser::ParseGraph(strict);
  ASSERT_FALSE(r1.ok());
  EXPECT_NE(r1.status().message().find("typo"), std::string::npos);

  const char* lenient = R"({"kind":"GraphSpec","schema_version":1,"allow_unknown_fields":true,"name":"g",
    "nodes":[{"id":"a","operator":"X@1.0.0","typo":1}],"edges":[],"extra":true})";
  const auto r2 = ge::GraphSpecParser::ParseGraph(lenient);
  ASSERT_TRUE(r2.ok()) << r2.status().ToString();
  EXPECT_TRUE(r2->allow_unknown_fields());
}

TEST(GraphSpecJsonTest, FieldValidation) {
  const auto bad = [](const char* nodes_or_edges) {
    std::string doc = R"({"kind":"GraphSpec","schema_version":1,"name":"g",)";
    doc += nodes_or_edges;
    doc += "}";
    return ge::GraphSpecParser::ParseGraph(doc).status().code();
  };
  EXPECT_EQ(bad(R"("nodes":[],"edges":[])"), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(bad(R"("nodes":[{"id":"a","operator":"NoVersion"}],"edges":[])"), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(bad(R"("nodes":[{"id":"a","operator":"X@1","executor":{"kind":"cpu","device_id":0}}],"edges":[])"), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(bad(R"("nodes":[{"id":"a","operator":"X@1","parallelism":0}],"edges":[])"), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(bad(R"("nodes":[{"id":"a","operator":"X@1"}],"edges":[{"from":"a.o","to":"a"}])"), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(bad(R"("nodes":[{"id":"a","operator":"X@1"}],"edges":[{"from":"a.o","to":"a.i","queue":{"policy":"maybe"}}])"), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(bad(R"("nodes":[{"id":"a","operator":"X@1"}],"edges":[{"from":"a.o","to":"a.i","sync":"never"}])"), GE_STATUS_GRAPH_INVALID);
}

constexpr const char* kPatchJson = R"({
  "kind": "MutationPatch",
  "schema_version": 1,
  "$id": "ge.dev/schema/patch/v1",
  "base_topology_version": 42,
  "remove_policy": "drain",
  "actions": [
    {"type": "insert_chain", "edge": "decoder-to-encoder",
     "nodes": [{"id": "scale", "operator": "VideoScale@1.0.0", "options": {"width": 1280}},
               {"id": "watermark", "operator": "Watermark@1.0.0"}],
     "ports": {"chain_input": "in", "chain_output": "out"}},
    {"type": "remove_chain", "nodes": ["scale", "watermark"], "mode": "bypass", "remove_policy": "drain"},
    {"type": "replace_node", "node": "detector",
     "replacement": {"operator": "Detector@2.1.0", "options": {"model": "det-v2.plan"}},
     "port_mapping": {"inputs": {"image": "image"}, "outputs": {"detections": "detections"}},
     "remove_policy": "fast"},
    {"type": "remove_branch", "entry_edge": "tee-to-720p", "nodes": ["scale_720p", "enc_720p"]},
    {"type": "add_node", "node": {"id": "n", "operator": "Op@1.0.0"}},
    {"type": "remove_node", "node": "n", "mode": "disconnect"},
    {"type": "add_edge", "edge": {"from": "a.out", "to": "b.in"}},
    {"type": "remove_edge", "edge": "a-to-b"},
    {"type": "set_node_options", "node": "encoder", "parameters": {"bitrate": 2500000}}
  ]
})";

TEST(MutationPatchJsonTest, ParsesAllNineActions) {
  const auto r = ge::GraphSpecParser::ParsePatch(kPatchJson);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  const ge::MutationPatch& p = *r;
  EXPECT_EQ(p.base_topology_version, 42U);
  EXPECT_EQ(p.remove_policy, ge::RemovePolicy::kDrain);
  ASSERT_EQ(p.actions.size(), 9U);
  const auto& ic = std::get<ge::InsertChainAction>(p.actions[0]);
  EXPECT_EQ(ic.edge, "decoder-to-encoder");
  ASSERT_EQ(ic.nodes.size(), 2U);
  EXPECT_EQ(ic.nodes[0].options.GetInteger("width"), 1280);
  EXPECT_EQ(ic.ports.chain_input, "in");
  const auto& rc = std::get<ge::RemoveChainAction>(p.actions[1]);
  EXPECT_EQ(rc.mode, ge::RemoveMode::kBypass);
  EXPECT_EQ(rc.remove_policy, ge::RemovePolicy::kDrain);
  const auto& rn = std::get<ge::ReplaceNodeAction>(p.actions[2]);
  EXPECT_EQ(rn.replacement_op.ToString(), "Detector@2.1.0");
  ASSERT_TRUE(rn.port_mapping.has_value());
  EXPECT_EQ(rn.port_mapping->inputs.at("image"), "image");
  EXPECT_EQ(rn.remove_policy, ge::RemovePolicy::kFast);
  const auto& rb = std::get<ge::RemoveBranchAction>(p.actions[3]);
  EXPECT_EQ(rb.nodes.size(), 2U);
  EXPECT_EQ(std::get<ge::AddEdgeAction>(p.actions[6]).edge.to.port, "in");
  EXPECT_EQ(std::get<ge::SetNodeOptionsAction>(p.actions[8]).parameters.GetInteger("bitrate"), 2500000);
}

TEST(MutationPatchJsonTest, RoundTripIsStable) {
  const auto r = ge::GraphSpecParser::ParsePatch(kPatchJson);
  ASSERT_TRUE(r.ok());
  const std::string once = ge::GraphSpecParser::SerializePatch(*r);
  const auto again = ge::GraphSpecParser::ParsePatch(once);
  ASSERT_TRUE(again.ok()) << again.status().ToString();
  EXPECT_EQ(*r, *again);
  EXPECT_EQ(once, ge::GraphSpecParser::SerializePatch(*again));
}

TEST(MutationPatchJsonTest, RejectsUnknownActionAndMissingFields) {
  EXPECT_EQ(ge::GraphSpecParser::ParsePatch(R"({"kind":"MutationPatch","schema_version":1,"actions":[{"type":"explode"}]})").status().code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(ge::GraphSpecParser::ParsePatch(R"({"kind":"MutationPatch","schema_version":1,"actions":[{"type":"insert_chain","edge":"e","nodes":[]}]})").status().code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(ge::GraphSpecParser::ParsePatch(R"({"kind":"MutationPatch","schema_version":1,"actions":[{"type":"remove_node","node":"n","mode":"sideways"}]})").status().code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(ge::GraphSpecParser::ParsePatch(R"({"kind":"MutationPatch","schema_version":1})").status().code(), GE_STATUS_GRAPH_INVALID);
}

}  // namespace
