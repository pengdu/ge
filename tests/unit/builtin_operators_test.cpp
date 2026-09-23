#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <ge/cpp/engine.h>
#include <ge/cpp/graph_spec.h>
#include <ge/cpp/graph_spec_json.h>
#include <ge/cpp/mutation_applier.h>
#include <ge/cpp/session.h>

#include "test_operators.h"

// P6 ge_core additions: builtin operators through the Engine
// (CompositeOperatorFactory), negotiated contracts at Open, operator
// events on the EventBus and multi-entry RemoveBranch.

namespace {

using namespace ge::test;

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

ge::PortCapability VideoPort(const char* name, ge::PortDirection dir, std::vector<std::string> pf,
                             ge::PortCardinality card = ge::PortCardinality::kSingle) {
  ge::PortCapability p;
  p.name = name;
  p.direction = dir;
  p.type_tag = "VideoFrame";
  p.cardinality = card;
  p.video = ge::VideoConstraints{};
  p.video->pixel_formats = std::move(pf);
  return p;
}

// Records what Open saw and publishes one event per packet.
class Probe final : public ge::Operator {
 public:
  ge::Status Open(const ge::OpenRequest& r) override {
    external_id = std::string(r.external_id);
    node_id = r.node_id;
    if (const ge::ConnectionContract* c = r.InputContract("in")) in_pix = c->video ? c->video->pixel_format : "";
    if (const ge::ConnectionContract* c = r.OutputContract("out")) out_pix = c->video ? c->video->pixel_format : "";
    input_ports = r.input_contracts.size();
    output_ports = r.output_contracts.size();
    return ge::Status::Ok();
  }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    for (const ge::PacketRef& in : req.inputs) {
      if (in->is_event()) continue;
      if (req.events != nullptr) {
        req.events->Publish("media_format_changed", ge::Severity::kInfo,
                            ge::JsonValue(ge::JsonObject{{"seq", ge::JsonValue(in->header.seq)}}));
      }
      ge::Packet out = *in;
      (void)req.sink->Emit("out", std::move(out));
    }
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }

  std::string external_id, in_pix, out_pix;
  ge::NodeId node_id = 0;
  std::size_t input_ports = 0, output_ports = 0;
};

// CountingSource emits Bytes; the video ports here need VideoFrame packets.
class VideoSource final : public ge::Operator {
 public:
  VideoSource(std::int64_t count, std::shared_ptr<ge::HostBufferPool> pool)
      : count_(count), pool_(std::move(pool)) {}
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    if (next_ >= count_) return ge::ProcessResult::kExhausted;
    ge::Packet p;
    p.header.seq = static_cast<ge::PacketSeq>(next_ + 1);
    p.header.type_tag = ge::TypeTagRegistry::Global().Intern("VideoFrame");
    p.payload = pool_->Allocate(8);
    std::memcpy(p.payload->data, &p.header.seq, sizeof(p.header.seq));
    const ge::Status s = req.sink->Emit("out", std::move(p));
    if (s.code() == GE_STATUS_WOULD_BLOCK) return ge::ProcessResult::kContinue;
    if (!s.ok()) return s;
    ++next_;
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }

 private:
  std::int64_t count_;
  std::shared_ptr<ge::HostBufferPool> pool_;
  std::int64_t next_ = 0;
};

struct Builtins {
  std::shared_ptr<ge::HostBufferPool> pool = ge::HostBufferPool::Create();
  std::shared_ptr<ge::BuiltinOperatorFactory> factory = std::make_shared<ge::BuiltinOperatorFactory>();
  std::vector<std::shared_ptr<ge::Operator>> keep;
  std::shared_ptr<Probe> probe = std::make_shared<Probe>();
  std::shared_ptr<Collector> sink = std::make_shared<Collector>();

  Builtins() {
    // Source declares NV12+P010, probe accepts both but emits NV12 only,
    // sink wants NV12: contracts are src.out=NV12 (fan-in of one), probe.out=NV12.
    factory->Register(Desc("VSrc@1.0.0", {}, {VideoPort("out", ge::PortDirection::kOutput, {"NV12", "P010"}, ge::PortCardinality::kMulti)}, true, 1),
                      [this](const ge::OperatorCreateArgs& a) {
                        const auto n = a.options.GetInteger("count").value_or(5);
                        auto s = std::make_shared<VideoSource>(n, pool);
                        return Keep(keep, s);
                      });
    factory->Register(Desc("Probe@1.0.0", {VideoPort("in", ge::PortDirection::kInput, {"NV12", "P010"})},
                           {VideoPort("out", ge::PortDirection::kOutput, {"NV12"}, ge::PortCardinality::kMulti)}, false, 1),
                      [this](const ge::OperatorCreateArgs&) { return Keep(keep, probe); });
    factory->Register(Desc("VSink@1.0.0", {VideoPort("in", ge::PortDirection::kInput, {"NV12"})}, {}),
                      [this](const ge::OperatorCreateArgs&) { return Keep(keep, sink); });
  }
};

ge::EngineConfig InlineConfig(const Builtins& b) {
  ge::EngineConfig c;
  c.cpu_threads = 0;
  c.watchdog_thread = false;
  c.builtin_operators = b.factory;
  return c;
}

bool RunToStop(ge::Engine& e, ge::Session* s) {
  for (int i = 0; i < 100000; ++i) {
    if (s->WaitStopped(std::chrono::milliseconds(0))) return true;
    (void)s->PumpMutations();
    e.Tick();
  }
  return false;
}

TEST(BuiltinOperatorsTest, EngineResolvesBuiltinsBeforePluginsAndPassesContracts) {
  Builtins b;
  auto engine = ge::Engine::Create(InlineConfig(b));
  ASSERT_TRUE(engine.ok()) << engine.status().ToString();
  ge::Engine& e = **engine;
  // GetCapability sees builtins.
  ASSERT_TRUE(e.GetCapability(Op("Probe@1.0.0")).ok());
  EXPECT_EQ(e.GetCapability(Op("Nope@1.0.0")).status().code(), GE_STATUS_NOT_FOUND);

  ge::GraphBuilder g("builtin");
  auto src = g.AddNode(Op("VSrc@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(7)}}));
  auto probe = g.AddNode(Op("Probe@1.0.0"), "probe");
  auto sink = g.AddNode(Op("VSink@1.0.0"), "sink");
  g.Connect(src.port("out"), probe.port("in"), {.id = "e0"});
  g.Connect(probe.port("out"), sink.port("in"), {.id = "e1"});

  std::vector<std::string> nodes;
  std::vector<std::int64_t> seqs;
  std::mutex mu;
  const ge::SubscriptionId sub = e.events().Subscribe(
      ge::EventFilter{.types = {"media_format_changed"}}, [&](const ge::Event& ev) {
        std::lock_guard lock(mu);
        nodes.push_back(ev.detail.GetString("node").value_or(""));
        seqs.push_back(ev.detail.GetInteger("seq").value_or(-1));
        EXPECT_EQ(ev.source_node, b.probe->node_id);
      });

  auto s = e.CreateSession(*g.Build());
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ASSERT_TRUE((*s)->Start().ok());
  ASSERT_TRUE(RunToStop(e, *s));
  EXPECT_EQ(b.sink->Seqs().size(), 7U) << (*s)->failure().ToString() << " state=" << static_cast<int>((*s)->state());

  // OpenRequest tail fields.
  EXPECT_EQ(b.probe->external_id, "probe");
  EXPECT_NE(b.probe->node_id, 0U);
  EXPECT_EQ(b.probe->input_ports, 1U);
  EXPECT_EQ(b.probe->output_ports, 1U);
  EXPECT_EQ(b.probe->in_pix, "NV12");
  EXPECT_EQ(b.probe->out_pix, "NV12");

  e.events().Flush();
  e.events().Cancel(sub);
  std::lock_guard lock(mu);
  ASSERT_EQ(seqs.size(), 7U);
  EXPECT_EQ(seqs.front(), 1);
  EXPECT_EQ(nodes.front(), "probe");
  ASSERT_TRUE(e.DestroySession((*s)->id()).ok());
}

TEST(BuiltinOperatorsTest, TeeThenConvertRepairsIncompatibleFanoutConsumer) {
  Builtins b;
  auto p010_sink = std::make_shared<Collector>();
  auto converter = std::make_shared<Probe>();
  b.factory->Register(Desc("P010Sink@1.0.0", {VideoPort("in", ge::PortDirection::kInput, {"P010"})}, {}),
                      [&b, p010_sink](const ge::OperatorCreateArgs&) { return Keep(b.keep, p010_sink); });
  b.factory->Register(
      Desc("Convert@1.0.0", {VideoPort("in", ge::PortDirection::kInput, {"NV12"})},
           {VideoPort("out", ge::PortDirection::kOutput, {"P010"}, ge::PortCardinality::kMulti)}, false, 1),
      [&b, converter](const ge::OperatorCreateArgs&) { return Keep(b.keep, converter); });

  auto engine = ge::Engine::Create(InlineConfig(b));
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;

  ge::GraphBuilder bad("bad_fanout");
  auto bad_src = bad.AddNode(Op("VSrc@1.0.0"), "src");
  auto bad_nv12 = bad.AddNode(Op("VSink@1.0.0"), "nv12");
  auto bad_p010 = bad.AddNode(Op("P010Sink@1.0.0"), "p010");
  bad.Connect(bad_src.port("out"), bad_nv12.port("in"), {.id = "nv12"});
  bad.Connect(bad_src.port("out"), bad_p010.port("in"), {.id = "p010"});
  EXPECT_EQ(e.CreateSession(*bad.Build()).status().code(), GE_STATUS_CAPABILITY_CONFLICT);

  ge::GraphBuilder repaired("tee_convert");
  auto src = repaired.AddNode(Op("VSrc@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(37)}}));
  auto nv12 = repaired.AddNode(Op("VSink@1.0.0"), "nv12");
  auto convert = repaired.AddNode(Op("Convert@1.0.0"), "convert");
  auto p010 = repaired.AddNode(Op("P010Sink@1.0.0"), "p010");
  repaired.Connect(src.port("out"), nv12.port("in"), {.id = "tee_nv12"});
  repaired.Connect(src.port("out"), convert.port("in"), {.id = "tee_convert"});
  repaired.Connect(convert.port("out"), p010.port("in"), {.id = "convert_p010"});
  auto session = e.CreateSession(*repaired.Build());
  ASSERT_TRUE(session.ok()) << session.status().ToString();
  ASSERT_TRUE((*session)->Start().ok());
  ASSERT_TRUE(RunToStop(e, *session));
  EXPECT_EQ(b.sink->Seqs().size(), 37U);
  EXPECT_EQ(p010_sink->Seqs().size(), 37U);
  EXPECT_EQ(converter->in_pix, "NV12");
  EXPECT_EQ(converter->out_pix, "P010");
}

TEST(BuiltinOperatorsTest, CompositeFactoryOrderAndFallthrough) {
  ge::BuiltinOperatorFactory a, b;
  a.Register(Desc("Same@1.0.0", {}, {}), [](const ge::OperatorCreateArgs&) { return std::make_unique<Probe>(); });
  b.Register(Desc("Same@1.0.0", {}, {}), [](const ge::OperatorCreateArgs&) { return std::make_unique<Collector>(); });
  b.Register(Desc("OnlyB@1.0.0", {}, {}), [](const ge::OperatorCreateArgs&) { return std::make_unique<Collector>(); });
  ge::CompositeOperatorFactory c;
  c.Add(&a);
  c.Add(&b);
  EXPECT_EQ(c.Describe(Op("Same@1.0.0")), a.Describe(Op("Same@1.0.0")));
  EXPECT_EQ(c.Describe(Op("OnlyB@1.0.0")), b.Describe(Op("OnlyB@1.0.0")));
  EXPECT_EQ(c.Describe(Op("None@1.0.0")), nullptr);
  ge::OperatorCreateArgs args;
  args.key = Op("Same@1.0.0");
  auto op = c.Create(args);
  ASSERT_TRUE(op.ok());
  EXPECT_NE(dynamic_cast<Probe*>(op->get()), nullptr);
  args.key = Op("None@1.0.0");
  EXPECT_EQ(c.Create(args).status().code(), GE_STATUS_NOT_FOUND);
}

// ---------------------------------------------------------------------------
// Multi-entry RemoveBranch (12 §7.6, P6): a rendition branch hangs off the
// video decoder and the shared audio encoder.
// ---------------------------------------------------------------------------

ge::GraphSpec RenditionGraph() {
  ge::GraphSpec s("rend");
  for (const char* id : {"demux", "vdec", "adec", "aenc", "scale", "venc", "mux", "scale2", "venc2", "mux2"}) {
    EXPECT_TRUE(s.AddNode({.id = id, .op = Op("X@1.0.0")}).ok());
  }
  EXPECT_TRUE(s.AddEdge({.id = "d-v", .from = {"demux", "video"}, .to = {"vdec", "in"}}).ok());
  EXPECT_TRUE(s.AddEdge({.id = "d-a", .from = {"demux", "audio"}, .to = {"adec", "in"}}).ok());
  EXPECT_TRUE(s.AddEdge({.id = "adec-aenc", .from = {"adec", "out"}, .to = {"aenc", "in"}}).ok());
  for (const char* sfx : {"", "2"}) {
    const std::string sc = std::string("scale") + sfx, ve = std::string("venc") + sfx, mx = std::string("mux") + sfx;
    EXPECT_TRUE(s.AddEdge({.id = "v-" + sc, .from = {"vdec", "out"}, .to = {sc, "in"}}).ok());
    EXPECT_TRUE(s.AddEdge({.id = sc + "-" + ve, .from = {sc, "out"}, .to = {ve, "in"}}).ok());
    EXPECT_TRUE(s.AddEdge({.id = ve + "-" + mx, .from = {ve, "out"}, .to = {mx, "video"}}).ok());
    EXPECT_TRUE(s.AddEdge({.id = "a-" + mx, .from = {"aenc", "out"}, .to = {mx, "audio"}}).ok());
  }
  return s;
}

TEST(MultiEntryRemoveBranchTest, SingleEntryReportsAudioEdgeAsSharedMultiEntrySucceeds) {
  ge::MutationApplier applier;
  const ge::GraphSpec s = RenditionGraph();
  ge::Mutation single;
  single.RemoveBranch("v-scale", {.nodes = {"scale", "venc", "mux"}});
  const auto bad = applier.Apply(s, single.patch());
  EXPECT_EQ(bad.status().code(), GE_STATUS_SHARED_DEPENDENCY);
  EXPECT_NE(bad.status().context_json().find("a-mux"), std::string::npos);

  ge::Mutation multi;
  multi.RemoveBranch(std::vector<std::string>{"v-scale", "a-mux"}, {.nodes = {"scale", "venc", "mux"}});
  const auto r = applier.Apply(s, multi.patch());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->removed_nodes, (std::vector<std::string>{"scale", "venc", "mux"}));
  EXPECT_EQ(r->removed_edges.size(), 4U);
  EXPECT_NE(r->candidate.FindEdge("a-mux2"), nullptr);  // other rendition untouched
  EXPECT_NE(r->candidate.FindEdge("v-scale2"), nullptr);
  EXPECT_EQ(r->candidate.FindEdge("a-mux"), nullptr);

  // Every entry must lead into the selection and start outside it.
  ge::Mutation wrong;
  wrong.RemoveBranch(std::vector<std::string>{"v-scale", "scale-venc"}, {.nodes = {"scale", "venc", "mux"}});
  EXPECT_EQ(applier.Apply(s, wrong.patch()).status().code(), GE_STATUS_GRAPH_INVALID);
  ge::Mutation missing;
  missing.RemoveBranch(std::vector<std::string>{"v-scale", "nope"}, {.nodes = {"scale", "venc", "mux"}});
  EXPECT_EQ(applier.Apply(s, missing.patch()).status().code(), GE_STATUS_NOT_FOUND);
}

TEST(MultiEntryRemoveBranchTest, JsonRoundTripKeepsEntryEdges) {
  ge::Mutation multi;
  multi.RemoveBranch(std::vector<std::string>{"v-scale", "a-mux"}, {.nodes = {"scale", "venc", "mux"}});
  const std::string json = ge::GraphSpecParser::SerializePatch(multi.patch());
  EXPECT_NE(json.find("\"entry_edges\""), std::string::npos);
  EXPECT_EQ(json.find("\"entry_edge\""), std::string::npos);
  auto parsed = ge::GraphSpecParser::ParsePatch(json);
  ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
  EXPECT_EQ(*parsed, multi.patch());

  ge::Mutation single;
  single.RemoveBranch("v-scale", {.nodes = {"scale"}});
  const std::string sj = ge::GraphSpecParser::SerializePatch(single.patch());
  EXPECT_NE(sj.find("\"entry_edge\""), std::string::npos);
  EXPECT_EQ(sj.find("\"entry_edges\""), std::string::npos);
  auto sp = ge::GraphSpecParser::ParsePatch(sj);
  ASSERT_TRUE(sp.ok());
  EXPECT_EQ(*sp, single.patch());

  // Neither field: rejected.
  auto none = ge::GraphSpecParser::ParsePatch(
      R"({"kind":"MutationPatch","schema_version":1,"actions":[{"type":"remove_branch","nodes":["scale"]}]})");
  EXPECT_FALSE(none.ok());
}

}  // namespace
