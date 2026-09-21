#include <ge/cpp/graph_validator.h>

#include <gtest/gtest.h>

#include <chrono>
#include <map>

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || __has_feature(address_sanitizer) || \
    __has_feature(thread_sanitizer)
#define GE_TEST_SANITIZED 1
#else
#define GE_TEST_SANITIZED 0
#endif

namespace {

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

ge::PortCapability Port(const char* name, ge::PortDirection dir, const char* tag,
                        std::vector<std::string> pf = {}) {
  ge::PortCapability p;
  p.name = name;
  p.direction = dir;
  p.type_tag = tag;
  if (!pf.empty()) {
    p.video = ge::VideoConstraints{};
    p.video->pixel_formats = std::move(pf);
  }
  return p;
}

class Registry {
 public:
  Registry() {
    Add("Src@1.0.0", {}, {Port("out", ge::PortDirection::kOutput, "VideoFrame", {"NV12", "P010"})});
    auto& src_multi = descs_[Op("Src@1.0.0")].outputs[0];
    src_multi.cardinality = ge::PortCardinality::kMulti;
    Add("Scale@1.0.0", {Port("in", ge::PortDirection::kInput, "VideoFrame", {"NV12", "P010"})},
        {Port("out", ge::PortDirection::kOutput, "VideoFrame", {"NV12"})});
    Add("Sink@1.0.0", {Port("in", ge::PortDirection::kInput, "VideoFrame", {"NV12"})}, {});
    Add("SinkP010@1.0.0", {Port("in", ge::PortDirection::kInput, "VideoFrame", {"P010"})}, {});
    Add("Mix@1.0.0",
        {Port("a", ge::PortDirection::kInput, "VideoFrame", {"NV12"}),
         Port("b", ge::PortDirection::kInput, "VideoFrame", {"NV12"})},
        {Port("out", ge::PortDirection::kOutput, "VideoFrame", {"NV12"})});
    auto& mix_b = descs_[Op("Mix@1.0.0")].inputs[1];
    mix_b.required = false;
    mix_b.cardinality = ge::PortCardinality::kMulti;
    descs_[Op("Mix@1.0.0")].outputs[0].cardinality = ge::PortCardinality::kMulti;
    Add("Tracker@1.0.0",
        {Port("in", ge::PortDirection::kInput, "VideoFrame", {"NV12"}),
         Port("state", ge::PortDirection::kInput, "biz.State@v1")},
        {Port("out", ge::PortDirection::kOutput, "VideoFrame", {"NV12"}),
         Port("state", ge::PortDirection::kOutput, "biz.State@v1")});
    descs_[Op("Tracker@1.0.0")].inputs[1].required = false;
    Add("Profile@1.0.0", {}, {Port("out", ge::PortDirection::kOutput, "biz.UserProfile@v1")});
    Add("ProfileSinkV1@1.0.0", {Port("in", ge::PortDirection::kInput, "biz.UserProfile@v1")}, {});
    Add("ProfileSinkV2@1.0.0", {Port("in", ge::PortDirection::kInput, "biz.UserProfile@v2")}, {});
    Add("GpuOnly@1.0.0", {Port("in", ge::PortDirection::kInput, "VideoFrame", {"NV12"})}, {});
    descs_[Op("GpuOnly@1.0.0")].execution.devices = {ge::DeviceKind::kGpu};
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
    d.execution.stateful = false;
    d.execution.max_parallelism = 4;
    descs_[d.op] = std::move(d);
  }
  std::map<ge::OperatorKey, ge::CapabilityDescriptor> descs_;
};

ge::GraphSpec Linear() {
  ge::GraphBuilder b("linear");
  const auto src = b.AddNode(Op("Src@1.0.0"), "src");
  const auto scale = b.AddNode(Op("Scale@1.0.0"), "scale");
  const auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  b.Connect(src.port("out"), scale.port("in"), {.id = "e1"});
  b.Connect(scale.port("out"), sink.port("in"), {.id = "e2"});
  return *b.Build();
}

TEST(GraphValidatorTest, LinearGraphProducesContractsAndOrder) {
  Registry reg;
  ge::GraphValidator v(reg.resolver());
  const auto r = v.Validate(Linear());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->topological_order, (std::vector<std::string>{"src", "scale", "sink"}));
  EXPECT_EQ(r->sources, (std::vector<std::string>{"src"}));
  EXPECT_EQ(r->sinks, (std::vector<std::string>{"sink"}));
  ASSERT_EQ(r->edge_contracts.size(), 2U);
  EXPECT_EQ(r->edge_contracts.at("e1").video->pixel_format, "NV12");
  EXPECT_EQ(r->edge_contracts.at("e2").video->pixel_format, "NV12");
  EXPECT_EQ(r->output_contracts.at("src.out"), r->edge_contracts.at("e1"));
}

TEST(GraphValidatorTest, ReportsAllStructuralIssues) {
  Registry reg;
  ge::GraphValidator v(reg.resolver());
  ge::GraphSpec spec("bad");
  ASSERT_TRUE(spec.AddNode({.id = "src", .op = Op("Src@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "ghost", .op = Op("Nope@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "sink", .op = Op("Sink@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "sink2", .op = Op("Sink@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "bad_port", .from = {"src", "nope"}, .to = {"sink", "in"}}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "bad_in", .from = {"src", "out"}, .to = {"sink", "wrong"}}).ok());
  const auto r = v.Validate(spec);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), GE_STATUS_NOT_FOUND);  // first issue: unknown operator
  const auto& issues = v.issues();
  ASSERT_GE(issues.size(), 4U);
  bool saw_port = false, saw_required = false, saw_ghost = false;
  for (const auto& i : issues) {
    if (i.edge_id == "bad_port") saw_port = true;
    if (i.node_id == "ghost") saw_ghost = true;
    if (i.node_id == "sink2" && i.message.find("required") != std::string::npos) saw_required = true;
  }
  EXPECT_TRUE(saw_port);
  EXPECT_TRUE(saw_ghost);
  EXPECT_TRUE(saw_required);
  EXPECT_NE(r.status().context_json().find("\"issues\""), std::string::npos);

  ge::ValidateOptions fast;
  fast.fail_fast = true;
  ASSERT_FALSE(v.Validate(spec, fast).ok());
  EXPECT_EQ(v.issues().size(), 1U);
}

TEST(GraphValidatorTest, CycleWithoutFeedbackRejected) {
  Registry reg;
  ge::GraphValidator v(reg.resolver());
  ge::GraphSpec spec("cycle");
  ASSERT_TRUE(spec.AddNode({.id = "src", .op = Op("Src@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "t", .op = Op("Tracker@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "sink", .op = Op("Sink@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "e1", .from = {"src", "out"}, .to = {"t", "in"}}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "e2", .from = {"t", "out"}, .to = {"sink", "in"}}).ok());
  ge::EdgeSpec loop{.id = "loop", .from = {"t", "state"}, .to = {"t", "state"}};
  loop.queue.policy = ge::DropPolicy::kDropOldest;
  ASSERT_TRUE(spec.AddEdge(loop).ok());
  auto r = v.Validate(spec);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_NE(r.status().message().find("feedback"), std::string::npos);

  spec.FindEdge("loop")->feedback = true;
  r = v.Validate(spec);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->topological_order, (std::vector<std::string>{"src", "t", "sink"}));
  EXPECT_EQ(r->edge_contracts.at("loop").logical_type, "biz.State@v1");

  spec.FindEdge("loop")->queue.policy = ge::DropPolicy::kBlock;
  r = v.Validate(spec);
  EXPECT_EQ(r.status().code(), GE_STATUS_GRAPH_INVALID);
}

TEST(GraphValidatorTest, CardinalityAndOptionalInputs) {
  Registry reg;
  ge::GraphValidator v(reg.resolver());
  ge::GraphSpec spec("mix");
  ASSERT_TRUE(spec.AddNode({.id = "s1", .op = Op("Src@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "s2", .op = Op("Src@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "mix", .op = Op("Mix@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "sink", .op = Op("Sink@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "a", .from = {"s1", "out"}, .to = {"mix", "a"}}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "o", .from = {"mix", "out"}, .to = {"sink", "in"}}).ok());
  EXPECT_TRUE(v.Validate(spec).ok()) << v.Validate(spec).status().ToString();
  ASSERT_TRUE(spec.AddEdge({.id = "a2", .from = {"s2", "out"}, .to = {"mix", "a"}}).ok());
  const auto r = v.Validate(spec);
  EXPECT_EQ(r.status().code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_NE(r.status().message().find("single"), std::string::npos);
}

TEST(GraphValidatorTest, ExecutorAndParallelismAgainstCapability) {
  Registry reg;
  ge::GraphValidator v(reg.resolver());
  ge::GraphSpec spec("exec");
  ASSERT_TRUE(spec.AddNode({.id = "src", .op = Op("Src@1.0.0")}).ok());
  ge::NodeSpec gpu{.id = "g", .op = Op("GpuOnly@1.0.0")};
  gpu.executor.kind = ge::ExecutorKind::kCpu;
  ASSERT_TRUE(spec.AddNode(gpu).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "e", .from = {"src", "out"}, .to = {"g", "in"}}).ok());
  EXPECT_EQ(v.Validate(spec).status().code(), GE_STATUS_GRAPH_INVALID);
  spec.FindNode("g")->executor.kind = ge::ExecutorKind::kGpu;
  spec.FindNode("g")->parallelism = 8;
  EXPECT_EQ(v.Validate(spec).status().code(), GE_STATUS_GRAPH_INVALID);
  spec.FindNode("g")->parallelism = 4;
  EXPECT_TRUE(v.Validate(spec).ok());
}

TEST(GraphValidatorTest, NegotiationFailureIncludesConverterSuggestion) {
  Registry reg;
  ge::GraphValidator v(reg.resolver());
  ge::GraphBuilder b("p010");
  const auto scale = b.AddNode(Op("Scale@1.0.0"), "scale");
  const auto src = b.AddNode(Op("Src@1.0.0"), "src");
  const auto sink = b.AddNode(Op("SinkP010@1.0.0"), "sink");
  b.Connect(src.port("out"), scale.port("in"), {.id = "e1"});
  b.Connect(scale.port("out"), sink.port("in"), {.id = "e2"});
  const auto r = v.Validate(*b.Build());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), GE_STATUS_CAPABILITY_CONFLICT);
  ASSERT_EQ(v.issues().size(), 1U);
  EXPECT_EQ(v.issues()[0].edge_id, "e2");
  EXPECT_NE(v.issues()[0].message.find("VideoConvert"), std::string::npos);
}

TEST(GraphValidatorTest, OpaqueTypeExactMatch) {
  Registry reg;
  ge::GraphValidator v(reg.resolver());
  ge::GraphBuilder ok("v1");
  ok.Connect(ok.AddNode(Op("Profile@1.0.0"), "p").port("out"),
             ok.AddNode(Op("ProfileSinkV1@1.0.0"), "s").port("in"), {.id = "e"});
  const auto r1 = v.Validate(*ok.Build());
  ASSERT_TRUE(r1.ok()) << r1.status().ToString();
  EXPECT_EQ(r1->edge_contracts.at("e").logical_type, "biz.UserProfile@v1");

  ge::GraphBuilder bad("v2");
  bad.Connect(bad.AddNode(Op("Profile@1.0.0"), "p").port("out"),
              bad.AddNode(Op("ProfileSinkV2@1.0.0"), "s").port("in"), {.id = "e"});
  EXPECT_EQ(v.Validate(*bad.Build()).status().code(), GE_STATUS_CAPABILITY_CONFLICT);
}

TEST(GraphValidatorTest, FanoutContractIndependentOfDeclarationOrder) {
  Registry reg;
  ge::GraphValidator v(reg.resolver());
  const auto build = [](bool swap) {
    ge::GraphBuilder b("fan");
    const auto src = b.AddNode(Op("Src@1.0.0"), "src");
    const auto a = b.AddNode(Op("Scale@1.0.0"), "a");
    const auto s = b.AddNode(Op("SinkP010@1.0.0"), "s");
    const auto sa = b.AddNode(Op("Sink@1.0.0"), "sa");
    if (swap) {
      b.Connect(src.port("out"), s.port("in"), {.id = "e_s"});
      b.Connect(src.port("out"), a.port("in"), {.id = "e_a"});
    } else {
      b.Connect(src.port("out"), a.port("in"), {.id = "e_a"});
      b.Connect(src.port("out"), s.port("in"), {.id = "e_s"});
    }
    b.Connect(a.port("out"), sa.port("in"), {.id = "e_sa"});
    return *b.Build();
  };
  const auto r1 = v.Validate(build(false));
  const auto r2 = v.Validate(build(true));
  ASSERT_TRUE(r1.ok()) << r1.status().ToString();
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(r1->output_contracts.at("src.out"), r2->output_contracts.at("src.out"));
  EXPECT_EQ(r1->output_contracts.at("src.out").video->pixel_format, "P010");
  EXPECT_EQ(r1->edge_contracts.at("e_a"), r1->edge_contracts.at("e_s"));
}

TEST(GraphValidatorTest, SharedDependenciesAndLinearChain) {
  ge::GraphSpec spec("shared");
  for (const char* id : {"src", "tee_a", "tee_b", "enc_a", "enc_b", "sink_a", "sink_b"}) {
    ASSERT_TRUE(spec.AddNode({.id = id, .op = Op("X@1.0.0")}).ok());
  }
  ASSERT_TRUE(spec.AddEdge({.id = "src-a", .from = {"src", "out"}, .to = {"tee_a", "in"}}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "src-b", .from = {"src", "out"}, .to = {"tee_b", "in"}}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "a1", .from = {"tee_a", "out"}, .to = {"enc_a", "in"}}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "a2", .from = {"enc_a", "out"}, .to = {"sink_a", "in"}}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "b1", .from = {"tee_b", "out"}, .to = {"enc_b", "in"}}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "b2", .from = {"enc_b", "out"}, .to = {"sink_b", "in"}}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "cross", .from = {"enc_a", "out"}, .to = {"sink_b", "in"}}).ok());

  EXPECT_TRUE(ge::GraphValidator::FindSharedDependencies(spec, {"tee_b", "enc_b", "sink_b"}, "src-b").empty() == false);
  const auto deps = ge::GraphValidator::FindSharedDependencies(spec, {"tee_b", "enc_b", "sink_b"}, "src-b");
  ASSERT_EQ(deps.size(), 1U);
  EXPECT_EQ(deps[0].edge_id, "cross");
  EXPECT_EQ(deps[0].node_id, "sink_b");
  EXPECT_EQ(deps[0].external_node, "enc_a");
  EXPECT_TRUE(ge::GraphValidator::FindSharedDependencies(spec, {"tee_a", "enc_a", "sink_a"}, "src-a").size() == 1U);

  const auto chain = ge::GraphValidator::CheckLinearChain(spec, {"tee_b", "enc_b"});
  ASSERT_TRUE(chain.ok()) << chain.status().ToString();
  EXPECT_EQ(chain->inbound_edge, "src-b");
  EXPECT_EQ(chain->outbound_edge, "b2");

  const auto shared = ge::GraphValidator::CheckLinearChain(spec, {"tee_a", "enc_a"});
  EXPECT_EQ(shared.status().code(), GE_STATUS_SHARED_DEPENDENCY);
  EXPECT_NE(shared.status().context_json().find("cross"), std::string::npos);
  EXPECT_EQ(ge::GraphValidator::CheckLinearChain(spec, {"enc_b", "tee_b"}).status().code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(ge::GraphValidator::CheckLinearChain(spec, {"nope"}).status().code(), GE_STATUS_NOT_FOUND);
  EXPECT_EQ(ge::GraphValidator::CheckLinearChain(spec, {}).status().code(), GE_STATUS_GRAPH_INVALID);
}

TEST(GraphValidatorTest, HundredNodesThreeHundredEdgesUnder50ms) {
  Registry reg;
  ge::GraphValidator v(reg.resolver());
  ge::GraphSpec spec("big");
  ASSERT_TRUE(spec.AddNode({.id = "src", .op = Op("Src@1.0.0")}).ok());
  for (int i = 0; i < 99; ++i) {
    ASSERT_TRUE(spec.AddNode({.id = "mix" + std::to_string(i), .op = Op("Mix@1.0.0")}).ok());
  }
  int edges = 0;
  for (int i = 0; i < 99; ++i) {
    const std::string from = i == 0 ? "src" : "mix" + std::to_string(i - 1);
    ASSERT_TRUE(spec.AddEdge({.from = {from, "out"}, .to = {"mix" + std::to_string(i), "a"}}).ok());
    ++edges;
  }
  for (int i = 1; i < 99 && edges < 300; ++i) {
    const std::string to = "mix" + std::to_string(i);
    for (int j = i - 1; j >= 0 && edges < 300 && j > i - 4; --j) {
      ASSERT_TRUE(spec.AddEdge({.from = {"mix" + std::to_string(j), "out"}, .to = {to, "b"}}).ok());
      ++edges;
    }
  }
  ASSERT_EQ(edges, 300);
  const auto start = std::chrono::steady_clock::now();
  const auto r = v.Validate(spec);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start).count();
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->topological_order.size(), 100U);
  // The 50ms budget (15 P1) is measured by ge_bench_validate on optimised
  // builds; here the check only guards against algorithmic blow-ups. ASan
  // on a shared CI runner has been seen at 566ms, so the sanitized bound is
  // deliberately loose.
  EXPECT_LT(ms, GE_TEST_SANITIZED ? 2000 : 50);
}

}  // namespace
