#include <ge/cpp/mutation_applier.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <map>

namespace {

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

ge::PortCapability Port(const char* name, ge::PortDirection dir, bool required = true) {
  ge::PortCapability p;
  p.name = name;
  p.direction = dir;
  p.type_tag = "VideoFrame";
  p.required = required;
  return p;
}

class Registry {
 public:
  Registry() {
    Add("Scale@1.0.0", {Port("in", ge::PortDirection::kInput)}, {Port("out", ge::PortDirection::kOutput)});
    Add("Watermark@1.0.0", {Port("in", ge::PortDirection::kInput)}, {Port("out", ge::PortDirection::kOutput)});
    Add("Mix@1.0.0",
        {Port("a", ge::PortDirection::kInput), Port("b", ge::PortDirection::kInput)},
        {Port("out", ge::PortDirection::kOutput)});
    Add("Detector@2.1.0", {Port("frame", ge::PortDirection::kInput)},
        {Port("boxes", ge::PortDirection::kOutput)});
    Add("Detector@2.2.0", {Port("image", ge::PortDirection::kInput)},
        {Port("detections", ge::PortDirection::kOutput)});
  }
  ge::CapabilityResolver resolver() {
    return [this](const ge::OperatorKey& k) -> const ge::CapabilityDescriptor* {
      const auto it = descs_.find(k);
      return it == descs_.end() ? nullptr : &it->second;
    };
  }

 private:
  void Add(const char* key, std::vector<ge::PortCapability> ins, std::vector<ge::PortCapability> outs) {
    ge::CapabilityDescriptor d;
    d.op = Op(key);
    d.inputs = std::move(ins);
    d.outputs = std::move(outs);
    descs_[d.op] = std::move(d);
  }
  std::map<ge::OperatorKey, ge::CapabilityDescriptor> descs_;
};

// src.out -> dec.in ; dec.out -> enc.in (edge "d2e") ; enc.out -> sink.in
ge::GraphSpec Pipeline() {
  ge::GraphSpec s("p");
  for (const char* id : {"src", "dec", "enc", "sink"}) {
    EXPECT_TRUE(s.AddNode({.id = id, .op = Op("X@1.0.0")}).ok());
  }
  EXPECT_TRUE(s.AddEdge({.id = "s2d", .from = {"src", "out"}, .to = {"dec", "in"}}).ok());
  ge::EdgeSpec d2e{.id = "d2e", .from = {"dec", "out"}, .to = {"enc", "in"}};
  d2e.queue.capacity = 8;
  d2e.queue.policy = ge::DropPolicy::kDropOldest;
  EXPECT_TRUE(s.AddEdge(d2e).ok());
  EXPECT_TRUE(s.AddEdge({.id = "e2s", .from = {"enc", "out"}, .to = {"sink", "in"}}).ok());
  return s;
}

TEST(MutationApplierTest, InsertChainSplitsEdgeAndKeepsQueue) {
  Registry reg;
  ge::MutationApplier applier(reg.resolver());
  ge::Mutation m;
  m.InsertChain("d2e", {{.id = "scale", .op = Op("Scale@1.0.0")},
                        {.id = "wm", .op = Op("Watermark@1.0.0")}});
  const auto r = applier.Apply(Pipeline(), m.patch());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  const ge::GraphSpec& c = r->candidate;
  EXPECT_EQ(c.FindEdge("d2e"), nullptr);
  EXPECT_EQ(c.nodes().size(), 6U);
  EXPECT_EQ(c.edges().size(), 5U);
  const ge::EdgeSpec* head = c.FindEdge("dec.out->scale.in");
  ASSERT_NE(head, nullptr);
  EXPECT_EQ(head->queue.capacity, 8U);
  EXPECT_EQ(head->queue.policy, ge::DropPolicy::kDropOldest);
  ASSERT_NE(c.FindEdge("scale.out->wm.in"), nullptr);
  ASSERT_NE(c.FindEdge("wm.out->enc.in"), nullptr);
  EXPECT_EQ(r->added_nodes, (std::vector<std::string>{"scale", "wm"}));
  EXPECT_EQ(r->removed_edges, (std::vector<std::string>{"d2e"}));
  EXPECT_EQ(r->added_edges.size(), 3U);
  EXPECT_TRUE(r->removed_nodes.empty());
}

TEST(MutationApplierTest, InsertChainNeedsExplicitPortsWhenAmbiguous) {
  Registry reg;
  ge::MutationApplier applier(reg.resolver());
  ge::Mutation ambiguous;
  ambiguous.InsertChain("d2e", {{.id = "mix", .op = Op("Mix@1.0.0")}});
  const auto bad = applier.Apply(Pipeline(), ambiguous.patch());
  EXPECT_EQ(bad.status().code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_NE(bad.status().context_json().find("insert_chain"), std::string::npos);

  ge::Mutation explicit_ports;
  explicit_ports.InsertChain("d2e", {{.id = "mix", .op = Op("Mix@1.0.0")}},
                             {.chain_input = "a", .chain_output = "out"});
  const auto ok = applier.Apply(Pipeline(), explicit_ports.patch());
  ASSERT_TRUE(ok.ok()) << ok.status().ToString();
  EXPECT_NE(ok->candidate.FindEdge("dec.out->mix.a"), nullptr);
  EXPECT_NE(ok->candidate.FindEdge("mix.out->enc.in"), nullptr);

  ge::Mutation dup;
  dup.InsertChain("d2e", {{.id = "enc", .op = Op("Scale@1.0.0")}});
  EXPECT_EQ(applier.Apply(Pipeline(), dup.patch()).status().code(), GE_STATUS_ALREADY_EXISTS);
  ge::Mutation missing;
  missing.InsertChain("nope", {{.id = "x", .op = Op("Scale@1.0.0")}});
  EXPECT_EQ(applier.Apply(Pipeline(), missing.patch()).status().code(), GE_STATUS_NOT_FOUND);
}

TEST(MutationApplierTest, RemoveChainBypassRestoresDirectEdge) {
  Registry reg;
  ge::MutationApplier applier(reg.resolver());
  ge::Mutation ins;
  ins.InsertChain("d2e", {{.id = "scale", .op = Op("Scale@1.0.0")},
                          {.id = "wm", .op = Op("Watermark@1.0.0")}});
  const auto with_chain = applier.Apply(Pipeline(), ins.patch());
  ASSERT_TRUE(with_chain.ok());

  ge::Mutation rm;
  rm.SetRemovePolicy(ge::RemovePolicy::kFast).RemoveChain({"scale", "wm"}, {.bypass = true});
  const auto r = applier.Apply(with_chain->candidate, rm.patch());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->candidate.nodes().size(), 4U);
  EXPECT_EQ(r->candidate.edges().size(), 3U);
  const ge::EdgeSpec* bypass = r->candidate.FindEdge("dec.out->enc.in");
  ASSERT_NE(bypass, nullptr);
  EXPECT_EQ(bypass->queue.capacity, 8U);
  EXPECT_EQ(r->removed_nodes, (std::vector<std::string>{"scale", "wm"}));
  EXPECT_EQ(r->node_remove_policy.at("scale"), ge::RemovePolicy::kFast);
  EXPECT_EQ(r->removed_edges.size(), 3U);
  EXPECT_EQ(r->added_edges, (std::vector<std::string>{"dec.out->enc.in"}));

  ge::Mutation wrong_order;
  wrong_order.RemoveChain({"wm", "scale"}, {.bypass = true});
  EXPECT_EQ(applier.Apply(with_chain->candidate, wrong_order.patch()).status().code(),
            GE_STATUS_GRAPH_INVALID);
}

TEST(MutationApplierTest, RemoveChainBypassRejectsSharedNodes) {
  Registry reg;
  ge::MutationApplier applier(reg.resolver());
  ge::GraphSpec s = Pipeline();
  ASSERT_TRUE(s.AddNode({.id = "tap", .op = Op("X@1.0.0")}).ok());
  ASSERT_TRUE(s.AddEdge({.id = "tap_edge", .from = {"dec", "out"}, .to = {"tap", "in"}}).ok());
  ge::Mutation rm;
  rm.RemoveChain({"dec"}, {.bypass = true});
  const auto r = applier.Apply(s, rm.patch());
  EXPECT_EQ(r.status().code(), GE_STATUS_SHARED_DEPENDENCY);
  EXPECT_NE(r.status().context_json().find("tap_edge"), std::string::npos);

  ge::Mutation disconnect;
  disconnect.RemoveChain({"dec"});
  const auto d = applier.Apply(s, disconnect.patch());
  ASSERT_TRUE(d.ok()) << d.status().ToString();
  EXPECT_EQ(d->candidate.FindNode("dec"), nullptr);
  EXPECT_EQ(d->removed_edges.size(), 3U);
  EXPECT_EQ(d->node_remove_policy.at("dec"), ge::RemovePolicy::kDrain);
}

TEST(MutationApplierTest, ReplaceNodeWithAndWithoutMapping) {
  Registry reg;
  ge::MutationApplier applier(reg.resolver());
  ge::GraphSpec s("det");
  ASSERT_TRUE(s.AddNode({.id = "src", .op = Op("X@1.0.0")}).ok());
  ASSERT_TRUE(s.AddNode({.id = "det", .op = Op("Detector@2.1.0")}).ok());
  ASSERT_TRUE(s.AddNode({.id = "sink", .op = Op("X@1.0.0")}).ok());
  ASSERT_TRUE(s.AddEdge({.id = "in", .from = {"src", "out"}, .to = {"det", "frame"}}).ok());
  ASSERT_TRUE(s.AddEdge({.id = "out", .from = {"det", "boxes"}, .to = {"sink", "in"}}).ok());

  ge::Mutation no_map;
  no_map.ReplaceNode("det", Op("Detector@2.2.0"));
  EXPECT_EQ(applier.Apply(s, no_map.patch()).status().code(), GE_STATUS_GRAPH_INVALID);

  ge::Mutation mapped;
  ge::PortMapping pm;
  pm.inputs["frame"] = "image";
  pm.outputs["boxes"] = "detections";
  mapped.ReplaceNode("det", Op("Detector@2.2.0"),
                     ge::JsonValue(ge::JsonObject{{"model", ge::JsonValue("v2.plan")}}), pm,
                     {.remove_policy = ge::RemovePolicy::kFast});
  const auto r = applier.Apply(s, mapped.patch());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  const ge::NodeSpec* n = r->candidate.FindNode("det");
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(n->op.ToString(), "Detector@2.2.0");
  EXPECT_EQ(n->options.GetString("model"), "v2.plan");
  ASSERT_NE(r->candidate.FindEdge("in"), nullptr);
  EXPECT_EQ(r->candidate.FindEdge("in")->to.port, "image");
  EXPECT_EQ(r->candidate.FindEdge("out")->from.port, "detections");
  EXPECT_EQ(r->removed_nodes, (std::vector<std::string>{"det"}));
  EXPECT_EQ(r->added_nodes, (std::vector<std::string>{"det"}));
  EXPECT_EQ(r->node_remove_policy.at("det"), ge::RemovePolicy::kFast);

  ge::Mutation same_name;
  same_name.ReplaceNode("det", Op("Detector@2.1.0"));
  EXPECT_TRUE(applier.Apply(s, same_name.patch()).ok());
}

TEST(MutationApplierTest, RemoveBranchRequiresExclusiveSubgraph) {
  ge::MutationApplier applier;
  ge::GraphSpec s("fan");
  for (const char* id : {"src", "tee", "s720", "e720", "k720", "s1080", "e1080", "k1080", "monitor"}) {
    ASSERT_TRUE(s.AddNode({.id = id, .op = Op("X@1.0.0")}).ok());
  }
  ASSERT_TRUE(s.AddEdge({.id = "src-tee", .from = {"src", "out"}, .to = {"tee", "in"}}).ok());
  ASSERT_TRUE(s.AddEdge({.id = "tee-720", .from = {"tee", "out"}, .to = {"s720", "in"}}).ok());
  ASSERT_TRUE(s.AddEdge({.id = "s720-e", .from = {"s720", "out"}, .to = {"e720", "in"}}).ok());
  ASSERT_TRUE(s.AddEdge({.id = "e720-k", .from = {"e720", "out"}, .to = {"k720", "in"}}).ok());
  ASSERT_TRUE(s.AddEdge({.id = "tee-1080", .from = {"tee", "out"}, .to = {"s1080", "in"}}).ok());
  ASSERT_TRUE(s.AddEdge({.id = "s1080-e", .from = {"s1080", "out"}, .to = {"e1080", "in"}}).ok());
  ASSERT_TRUE(s.AddEdge({.id = "e1080-k", .from = {"e1080", "out"}, .to = {"k1080", "in"}}).ok());
  ASSERT_TRUE(s.AddEdge({.id = "e720-mon", .from = {"e720", "out"}, .to = {"monitor", "a"}}).ok());
  ASSERT_TRUE(s.AddEdge({.id = "e1080-mon", .from = {"e1080", "out"}, .to = {"monitor", "b"}}).ok());

  // e720 also feeds the shared monitor: not exclusive.
  ge::Mutation shared;
  shared.RemoveBranch("tee-720", {.nodes = {"s720", "e720", "k720"}});
  const auto bad = applier.Apply(s, shared.patch());
  EXPECT_EQ(bad.status().code(), GE_STATUS_SHARED_DEPENDENCY);
  EXPECT_NE(bad.status().context_json().find("e720-mon"), std::string::npos);

  // Pulling the monitor into the selection exposes the 1080 side instead.
  ge::Mutation wider;
  wider.RemoveBranch("tee-720", {.nodes = {"s720", "e720", "k720", "monitor"}});
  const auto bad2 = applier.Apply(s, wider.patch());
  EXPECT_EQ(bad2.status().code(), GE_STATUS_SHARED_DEPENDENCY);
  EXPECT_NE(bad2.status().context_json().find("e1080-mon"), std::string::npos);

  ASSERT_TRUE(s.RemoveEdge("e720-mon").ok());
  ge::Mutation ok;
  ok.RemoveBranch("tee-720", {.nodes = {"s720", "e720", "k720"}});
  const auto r = applier.Apply(s, ok.patch());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->candidate.nodes().size(), 6U);
  EXPECT_NE(r->candidate.FindEdge("tee-1080"), nullptr);
  EXPECT_EQ(r->candidate.FindEdge("tee-720"), nullptr);
  EXPECT_EQ(r->removed_nodes, (std::vector<std::string>{"s720", "e720", "k720"}));
  EXPECT_EQ(r->removed_edges.size(), 3U);

  ge::Mutation unreachable;
  unreachable.RemoveBranch("tee-720", {.nodes = {"s720", "s1080"}});
  EXPECT_EQ(applier.Apply(s, unreachable.patch()).status().code(), GE_STATUS_GRAPH_INVALID);
  ge::Mutation wrong_entry;
  wrong_entry.RemoveBranch("src-tee", {.nodes = {"s720"}});
  EXPECT_EQ(applier.Apply(s, wrong_entry.patch()).status().code(), GE_STATUS_GRAPH_INVALID);
}

TEST(MutationApplierTest, SimpleActionsAndTracking) {
  ge::MutationApplier applier;
  ge::Mutation m;
  m.AddNode({.id = "n", .op = Op("Y@1.0.0")})
      .AddEdge({"enc", "out"}, {"n", "in"}, {.id = "to_n"})
      .RemoveEdge("to_n")
      .RemoveNode("n")
      .RemoveEdge("e2s")
      .SetNodeOptions("enc", ge::JsonValue(ge::JsonObject{{"bitrate", ge::JsonValue(1)}}));
  const auto r = applier.Apply(Pipeline(), m.patch());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_TRUE(r->added_nodes.empty());
  EXPECT_TRUE(r->removed_nodes.empty());
  EXPECT_TRUE(r->added_edges.empty());
  EXPECT_EQ(r->removed_edges, (std::vector<std::string>{"e2s"}));
  ASSERT_EQ(r->parameter_updates.size(), 1U);
  EXPECT_EQ(r->parameter_updates[0].node, "enc");
  EXPECT_EQ(r->candidate.nodes().size(), 4U);
  EXPECT_EQ(r->candidate.edges().size(), 2U);
  for (const char* id : {"n", "enc", "sink"}) {
    EXPECT_NE(std::find(r->touched_nodes.begin(), r->touched_nodes.end(), id), r->touched_nodes.end()) << id;
  }

  ge::Mutation bad;
  bad.AddNode({.id = "a", .op = Op("Y@1.0.0")}).RemoveNode("zzz");
  const auto e = applier.Apply(Pipeline(), bad.patch());
  EXPECT_EQ(e.status().code(), GE_STATUS_NOT_FOUND);
  EXPECT_NE(e.status().message().find("action[1] remove_node"), std::string::npos);
  EXPECT_EQ(applier.Apply(Pipeline(), ge::MutationPatch{}).status().code(), GE_STATUS_INVALID_ARGUMENT);
}

}  // namespace
