#include <ge/cpp/session.h>

#include <gtest/gtest.h>

#include <chrono>
#include <numeric>
#include <thread>

#include "test_operators.h"

namespace {

using namespace ge::test;

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

// Source that never exhausts on its own; the tests stop it via EOS/fast.
class TickingSource final : public ge::Operator {
 public:
  TickingSource(std::shared_ptr<ge::HostBufferPool> pool, std::chrono::microseconds period)
      : pool_(std::move(pool)), period_(period) {}
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    if (period_.count() > 0) std::this_thread::sleep_for(period_);
    ge::Packet p;
    p.header.seq = static_cast<ge::PacketSeq>(++next_);
    p.header.pts_ns = next_;
    p.header.type_tag = ge::TypeTagRegistry::Global().Intern("Bytes");
    p.payload = pool_->Allocate(8);
    ge::Status s = req.sink->Emit("out", std::move(p));
    if (s.code() == GE_STATUS_WOULD_BLOCK) --next_;
    else if (!s.ok()) return s;
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override {
    closed = true;
    return ge::Status::Ok();
  }
  std::atomic<bool> closed{false};
  ge::PacketSeq produced() const { return static_cast<ge::PacketSeq>(next_); }

 private:
  std::shared_ptr<ge::HostBufferPool> pool_;
  std::chrono::microseconds period_;
  std::int64_t next_ = 0;
};

// Tags every packet with its node id in metadata order: Collector records
// ports; we identify the path by the parameter "tag" the PassThrough saw.
struct Fixture {
  std::shared_ptr<ge::HostBufferPool> pool = ge::HostBufferPool::Create();
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  ge::OperationRegistry ops;
  std::map<std::string, CountingSource*> sources;
  std::map<std::string, TickingSource*> tickers;
  std::map<std::string, PassThrough*> passes;
  std::map<std::string, Collector*> sinks;
  std::map<std::string, Gate*> gates;

  Fixture() {
    factory.Register(Desc("Gate@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)}, false, 1),
                     [this](const ge::OperatorCreateArgs& a) {
                       auto g = std::make_shared<Gate>();
                       gates[a.external_id] = g.get();
                       return Keep(keep, g);
                     });
    factory.Register(Desc("Src@1.0.0", {}, {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)}, true, 1),
                     [this](const ge::OperatorCreateArgs& a) {
                       const auto n = a.options.GetInteger("count").value_or(10);
                       auto s = std::make_shared<CountingSource>(n, pool);
                       sources[a.external_id] = s.get();
                       return Keep(keep, s);
                     });
    factory.Register(Desc("Tick@1.0.0", {}, {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)}, true, 1),
                     [this](const ge::OperatorCreateArgs& a) {
                       const auto us = a.options.GetInteger("period_us").value_or(50);
                       auto s = std::make_shared<TickingSource>(pool, std::chrono::microseconds(us));
                       tickers[a.external_id] = s.get();
                       return Keep(keep, s);
                     });
    auto pass_desc = Desc("Pass@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)},
                          false, 1);
    pass_desc.parameters.migratable = {"gain"};
    factory.Register(pass_desc, [this](const ge::OperatorCreateArgs& a) {
      const auto us = a.options.GetInteger("delay_us").value_or(0);
      auto p = std::make_shared<PassThrough>(std::chrono::microseconds(us));
      passes[a.external_id] = p.get();
      return Keep(keep, p);
    });
    auto pass2 = pass_desc;
    pass2.op = Op("Pass@2.0.0");
    factory.Register(pass2, [this](const ge::OperatorCreateArgs& a) {
      auto p = std::make_shared<PassThrough>();
      passes[a.external_id + "@2"] = p.get();
      return Keep(keep, p);
    });
    factory.Register(Desc("Sink@1.0.0", {BytesPort("in", ge::PortDirection::kInput)}, {}),
                     [this](const ge::OperatorCreateArgs& a) {
                       auto c = std::make_shared<Collector>();
                       sinks[a.external_id] = c.get();
                       return Keep(keep, c);
                     });
    factory.Register(Desc("Faulty@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput)}, false, 1),
                     [](const ge::OperatorCreateArgs& a) {
                       return std::make_unique<Faulty>(static_cast<int>(a.options.GetInteger("fail_at").value_or(1)));
                     });
    factory.Register(Desc("BadOpen@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput)}),
                     [](const ge::OperatorCreateArgs&) { return std::make_unique<FailsToOpen>(); });
  }

  std::unique_ptr<ge::Session> Create(const ge::GraphSpec& spec, ge::ExecutorPool& exec,
                                      ge::SessionOptions options = {}, ge::SessionEvents events = {}) {
    if (exec.cpu_threads() == 0) options.coordinator_thread = false;
    auto r = ge::Session::Create(spec, factory, exec, ops, std::move(options), std::move(events));
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return r.ok() ? std::move(*r) : nullptr;
  }
};

ge::NodeSpec Node(const char* id, const char* op, ge::JsonValue options = ge::JsonValue(ge::JsonObject{})) {
  ge::NodeSpec n;
  n.id = id;
  n.op = Op(op);
  n.options = std::move(options);
  return n;
}

// src -> p0 -> sink  (source: CountingSource(count) or Tick when count<0)
ge::GraphSpec Linear(std::int64_t count, std::uint32_t capacity = 64) {
  ge::GraphBuilder b("linear");
  auto src = count < 0 ? b.AddNode(Op("Tick@1.0.0"), "src")
                       : b.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(count)}}));
  auto p0 = b.AddNode(Op("Pass@1.0.0"), "p0");
  auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  ge::EdgeOptions eo;
  eo.queue.capacity = capacity;
  b.Connect(src.port("out"), p0.port("in"), {.id = "e0", .queue = eo.queue});
  b.Connect(p0.port("out"), sink.port("in"), {.id = "e1", .queue = eo.queue});
  return *b.Build();
}

std::vector<ge::PacketSeq> Iota(std::int64_t n) {
  std::vector<ge::PacketSeq> v(static_cast<std::size_t>(n));
  std::iota(v.begin(), v.end(), 1U);
  return v;
}

// Drives an inline executor + host-pumped coordinator until quiet.
void Drain(ge::ExecutorPool& exec, ge::Session& s) {
  for (int i = 0; i < 100000; ++i) {
    bool did = s.PumpMutations();
    did = exec.RunPending() || did;
    s.Tick();
    if (!did && !exec.RunPending()) return;
  }
}

ge::OperationRecord WaitOp(ge::OperationRegistry& ops, ge::OperationId id, int ms = 5000) {
  auto r = ops.Wait(id, std::chrono::milliseconds(ms));
  EXPECT_TRUE(r.has_value()) << "operation " << id << " did not finish";
  return r.value_or(ge::OperationRecord{});
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(SessionTest, LifecycleStateMachine) {
  Fixture f;
  ge::ExecutorPool exec(0);
  std::vector<std::pair<ge::SessionState, ge::SessionState>> transitions;
  auto s = f.Create(Linear(20), exec, {}, {.on_state_changed = [&](auto a, auto b) { transitions.emplace_back(a, b); }});
  ASSERT_TRUE(s);
  EXPECT_EQ(s->state(), ge::SessionState::kCreated);
  EXPECT_EQ(s->Pause().code(), GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(s->Apply(ge::Mutation().RemoveNode("p0").Build()).status().code(), GE_STATUS_INVALID_ARGUMENT);
  ASSERT_TRUE(s->Start().ok());
  EXPECT_EQ(s->state(), ge::SessionState::kRunning);
  EXPECT_EQ(s->Start().code(), GE_STATUS_INVALID_ARGUMENT);
  ASSERT_TRUE(s->Pause().ok());
  EXPECT_EQ(s->state(), ge::SessionState::kPaused);
  ASSERT_TRUE(s->Resume().ok());
  Drain(exec, *s);
  // Natural end: every node closed -> stopped without an explicit Stop.
  EXPECT_EQ(s->state(), ge::SessionState::kStopped);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(20));
  auto op = s->Stop(false);
  ASSERT_TRUE(op.ok());
  EXPECT_EQ(WaitOp(f.ops, *op).state, ge::OperationState::kSucceeded);
  ASSERT_GE(transitions.size(), 5U);
  EXPECT_EQ(transitions[0].second, ge::SessionState::kStarting);
  EXPECT_EQ(transitions[1].second, ge::SessionState::kRunning);
  EXPECT_EQ(transitions.back().second, ge::SessionState::kStopped);
}

TEST(SessionTest, StopDrainAndFastComplete) {
  Fixture f;
  ge::ExecutorPool exec(3);
  {
    auto s = f.Create(Linear(-1), exec);
    ASSERT_TRUE(s->Start().ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto op = s->Stop(false);
    ASSERT_TRUE(op.ok());
    // The drain may already have finished on a starved machine.
    const ge::SessionState after_stop = s->state();
    EXPECT_TRUE(after_stop == ge::SessionState::kStopping || after_stop == ge::SessionState::kStopped);
    EXPECT_EQ(WaitOp(f.ops, *op).state, ge::OperationState::kSucceeded);
    EXPECT_EQ(s->state(), ge::SessionState::kStopped);
    EXPECT_TRUE(f.sinks["sink"]->flushed);
    const auto seqs = f.sinks["sink"]->Seqs();
    EXPECT_EQ(seqs, Iota(static_cast<std::int64_t>(seqs.size())));
  }
  {
    auto s = f.Create(Linear(-1), exec);
    ASSERT_TRUE(s->Start().ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto op = s->Stop(true);
    ASSERT_TRUE(op.ok());
    EXPECT_EQ(WaitOp(f.ops, *op).state, ge::OperationState::kSucceeded);
    EXPECT_TRUE(s->WaitStopped(std::chrono::milliseconds(1000)));
    EXPECT_TRUE(f.sinks["sink"]->fast_close);
  }
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

TEST(SessionTest, NodeFailureFailsSessionOnly) {
  Fixture f;
  ge::GraphBuilder b("f");
  auto src = b.AddNode(Op("Tick@1.0.0"), "src");
  auto bad = b.AddNode(Op("Faulty@1.0.0"), "bad", ge::JsonValue(ge::JsonObject{{"fail_at", ge::JsonValue(5)}}));
  auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  b.Connect(src.port("out"), bad.port("in"));
  b.Connect(bad.port("out"), sink.port("in"));
  ge::ExecutorPool exec(2);
  std::string failed;
  auto s = f.Create(*b.Build(), exec, {}, {.on_node_failed = [&](ge::NodeRuntime& n, const ge::Status&) { failed = n.external_id(); }});
  auto other = f.Create(Linear(-1), exec, {.id = 2});
  ASSERT_TRUE(s->Start().ok());
  ASSERT_TRUE(other->Start().ok());
  ASSERT_TRUE(s->WaitStopped(std::chrono::milliseconds(5000)));
  EXPECT_EQ(s->state(), ge::SessionState::kFailed);
  EXPECT_EQ(failed, "bad");
  EXPECT_EQ(s->failure().code(), GE_STATUS_INTERNAL);
  EXPECT_EQ(s->Apply(ge::Mutation().RemoveNode("bad").Build()).status().code(), GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(other->state(), ge::SessionState::kRunning);
  auto op = other->Stop(true);
  ASSERT_TRUE(op.ok());
  WaitOp(f.ops, *op);
  auto op2 = s->Stop(true);  // failed -> stop allowed
  ASSERT_TRUE(op2.ok());
  EXPECT_EQ(WaitOp(f.ops, *op2).state, ge::OperationState::kSucceeded);
}

// ---------------------------------------------------------------------------
// Parameters (12 §5, 15 §6 "N 用旧值、N+1 用新值；1000 次连续更新无版本错配")
// ---------------------------------------------------------------------------

TEST(SessionTest, ParameterUpdateEffectiveAtNextPacket) {
  Fixture f;
  ge::ExecutorPool exec(0);
  auto s = f.Create(Linear(200), exec);
  ASSERT_TRUE(s->Start().ok());
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(exec.RunOne());
  PassThrough* p0 = f.passes["p0"];
  const auto before = p0->Records();
  ASSERT_FALSE(before.empty());
  const ge::PacketSeq n = before.back().seq;
  auto upd = s->SetParameters("p0", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(7)}}));
  ASSERT_TRUE(upd.ok()) << upd.status().ToString();
  EXPECT_EQ(upd->version, 2U);
  EXPECT_EQ(WaitOp(f.ops, upd->operation).state, ge::OperationState::kSucceeded);
  // PAR-6: non hot-updatable key rejected, old values kept.
  auto bad = s->SetParameters("p0", ge::JsonValue(ge::JsonObject{{"model", ge::JsonValue("x")}}));
  EXPECT_EQ(bad.status().code(), GE_STATUS_PARAMETER_UNSUPPORTED);
  EXPECT_EQ(s->SetParameters("nope", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(1)}})).status().code(), GE_STATUS_NOT_FOUND);
  Drain(exec, *s);
  for (const auto& r : p0->Records()) {
    if (r.seq <= n) {
      EXPECT_EQ(r.parameter_version, 1U) << r.seq;
      EXPECT_EQ(r.gain, 0) << r.seq;
    } else {
      EXPECT_EQ(r.parameter_version, 2U) << r.seq;
      EXPECT_EQ(r.gain, 7) << r.seq;
    }
  }
  const auto hist = s->current_topology()->FindNode("p0")->parameters().history();
  ASSERT_EQ(hist.size(), 1U);
  EXPECT_EQ(hist[0].version, 2U);
  EXPECT_GT(hist[0].effective_after_seq, n);
}

TEST(SessionTest, ThousandParameterUpdatesNoVersionMismatch) {
  Fixture f;
  ge::ExecutorPool exec(3);
  ge::GraphBuilder b("p");
  auto src = b.AddNode(Op("Tick@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"period_us", ge::JsonValue(5)}}));
  auto p0 = b.AddNode(Op("Pass@1.0.0"), "p0");
  auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  b.Connect(src.port("out"), p0.port("in"));
  b.Connect(p0.port("out"), sink.port("in"));
  auto s = f.Create(*b.Build(), exec);
  ASSERT_TRUE(s->Start().ok());
  ge::ParameterVersion last = 1;
  for (int i = 1; i <= 1000; ++i) {
    auto u = s->SetParameters("p0", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(i)}}));
    ASSERT_TRUE(u.ok()) << u.status().ToString();
    EXPECT_EQ(u->version, last + 1);  // PAR-5: strictly serial
    last = u->version;
    if (i % 50 == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  // One switch per packet boundary (12 §5): wait until the queue caught up.
  ge::NodeRuntime* node = s->current_topology()->FindNode("p0");
  for (int i = 0; i < 2000 && node->parameters().Current()->version < 1001; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  auto op = s->Stop(false);
  ASSERT_TRUE(op.ok());
  WaitOp(f.ops, *op);
  // Every record: version v <=> gain == v-1 (version 1 == initial gain 0).
  ge::ParameterVersion prev = 0;
  for (const auto& r : f.passes["p0"]->Records()) {
    EXPECT_EQ(r.gain, static_cast<std::int64_t>(r.parameter_version) - 1) << r.seq;
    EXPECT_GE(r.parameter_version, prev);  // monotone
    prev = r.parameter_version;
  }
  EXPECT_EQ(f.passes["p0"]->Records().back().parameter_version, 1001U);
}

// ---------------------------------------------------------------------------
// Mutations (12 §7)
// ---------------------------------------------------------------------------

TEST(SessionTest, InsertChainIsAtomicAndKeepsOrder) {
  Fixture f;
  ge::ExecutorPool exec(0);
  auto s = f.Create(Linear(300), exec);
  ASSERT_TRUE(s->Start().ok());
  for (int i = 0; i < 9; ++i) ASSERT_TRUE(exec.RunOne());
  const std::size_t before = f.sinks["sink"]->Seqs().size();
  ge::TopologyVersion published = 0;
  s->events().on_topology_published = [&](ge::TopologyVersion v) {
    published = v;
    // Never a half-inserted graph: at publish time the chain is complete.
    const auto topo = s->current_topology();
    EXPECT_NE(topo->FindNode("a"), nullptr);
    EXPECT_NE(topo->FindNode("b"), nullptr);
    EXPECT_NE(topo->FindEdge("src.out->a.in"), nullptr);
    EXPECT_NE(topo->FindEdge("a.out->b.in"), nullptr);
    EXPECT_NE(topo->FindEdge("b.out->p0.in"), nullptr);
    EXPECT_EQ(topo->FindEdge("e0"), nullptr);
  };
  auto op = s->Apply(ge::Mutation().SetBaseVersion(1).InsertChain("e0", {Node("a", "Pass@1.0.0"), Node("b", "Pass@1.0.0")}).Build());
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  Drain(exec, *s);
  const auto rec = WaitOp(f.ops, *op);
  EXPECT_EQ(rec.state, ge::OperationState::kSucceeded) << rec.result.ToString();
  EXPECT_EQ(rec.topology_version, 2U);
  EXPECT_EQ(published, 2U);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(300));  // no loss, no reorder across the swap
  EXPECT_GT(f.sinks["sink"]->Seqs().size(), before);
  ASSERT_NE(f.passes.find("a"), f.passes.end());
  EXPECT_TRUE(f.passes["a"]->opened);
  EXPECT_FALSE(f.passes["a"]->Records().empty());
  EXPECT_EQ(f.passes["a"]->Records().size(), f.passes["b"]->Records().size());
  // Old p0 records carry V1, new ones V2 (12 §4.1 topology_version stamp).
  bool saw_v1 = false, saw_v2 = false;
  for (const auto& r : f.passes["p0"]->Records()) {
    saw_v1 = saw_v1 || r.topology_version == 1;
    saw_v2 = saw_v2 || r.topology_version == 2;
  }
  EXPECT_TRUE(saw_v1);
  EXPECT_TRUE(saw_v2);
  EXPECT_EQ(s->state(), ge::SessionState::kStopped);
  EXPECT_EQ(s->scheduler().pending_retirements(), 0U);
}

TEST(SessionTest, ConsecutiveInsertsNeverHalfInserted) {
  Fixture f;
  ge::ExecutorPool exec(3);
  auto s = f.Create(Linear(-1, 256), exec);
  ASSERT_TRUE(s->Start().ok());
  std::vector<ge::OperationId> ids;
  std::string edge = "e0";
  for (int i = 0; i < 5; ++i) {
    const std::string id = "n" + std::to_string(i);
    auto op = s->Apply(ge::Mutation().InsertChain(edge, {Node(id.c_str(), "Pass@1.0.0")}).Build());
    ASSERT_TRUE(op.ok());
    ids.push_back(*op);
    edge = id + ".out->p0.in";
  }
  for (const ge::OperationId id : ids) {
    const auto rec = WaitOp(f.ops, id);
    EXPECT_EQ(rec.state, ge::OperationState::kSucceeded) << rec.result.ToString();
  }
  EXPECT_EQ(s->topology_version(), 6U);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  auto stop = s->Stop(false);
  ASSERT_TRUE(stop.ok());
  WaitOp(f.ops, *stop);
  const auto seqs = f.sinks["sink"]->Seqs();
  EXPECT_EQ(seqs, Iota(static_cast<std::int64_t>(seqs.size())));
  for (int i = 0; i < 5; ++i) EXPECT_FALSE(f.passes["n" + std::to_string(i)]->Records().empty()) << i;
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

TEST(SessionTest, RemoveChainBypassOldPacketsTakeOldPath) {
  Fixture f;
  ge::ExecutorPool exec(0);
  // src -> a -> b -> sink, capacity 8 so packets queue up inside the chain.
  ge::GraphBuilder g("bypass");
  auto src = g.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(200)}}));
  auto a = g.AddNode(Op("Pass@1.0.0"), "a");
  auto b = g.AddNode(Op("Pass@1.0.0"), "b");
  auto sink = g.AddNode(Op("Sink@1.0.0"), "sink");
  g.Connect(src.port("out"), a.port("in"), {.id = "e0"});
  g.Connect(a.port("out"), b.port("in"), {.id = "e1"});
  g.Connect(b.port("out"), sink.port("in"), {.id = "e2"});
  auto s = f.Create(*g.Build(), exec);
  ASSERT_TRUE(s->Start().ok());
  // Let the source run ahead: pump only the source task a few times.
  for (int i = 0; i < 12; ++i) ASSERT_TRUE(exec.RunOne());
  const std::size_t a_before = f.passes["a"]->Records().size();
  const std::size_t b_before = f.passes["b"]->Records().size();
  auto op = s->Apply(ge::Mutation().RemoveChain({"a", "b"}, {.bypass = true}).Build());
  ASSERT_TRUE(op.ok());
  ASSERT_TRUE(s->PumpMutations());  // publish now, with packets queued in e0/e1
  EXPECT_EQ(s->topology_version(), 2U);
  EXPECT_NE(s->current_topology()->FindEdge("src.out->sink.in"), nullptr);
  EXPECT_EQ(s->current_topology()->FindNode("a"), nullptr);
  Drain(exec, *s);
  const auto rec = WaitOp(f.ops, *op);
  EXPECT_EQ(rec.state, ge::OperationState::kSucceeded) << rec.result.ToString();
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(200));  // old via chain + new via bypass, in order
  // In-flight packets finished the old chain (a/b saw more after publish).
  EXPECT_GE(f.passes["a"]->Records().size(), a_before);
  EXPECT_EQ(f.passes["a"]->Records().size(), f.passes["b"]->Records().size());
  EXPECT_LT(f.passes["b"]->Records().size(), 200U);  // rest bypassed
  EXPECT_GE(f.passes["b"]->Records().size(), b_before);
  EXPECT_TRUE(f.passes["a"]->flushed);
  EXPECT_TRUE(f.passes["a"]->closed);
  EXPECT_FALSE(f.passes["a"]->fast_close);
  EXPECT_TRUE(f.passes["b"]->closed);
  EXPECT_EQ(s->scheduler().pending_retirements(), 0U);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

TEST(SessionTest, SharedDependencyRejectedWithList) {
  Fixture f;
  ge::ExecutorPool exec(0);
  // src -> a -> {sink1, b -> sink2}; removing branch {a, sink1} shares a.out with b.
  ge::GraphBuilder g("shared");
  auto src = g.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(50)}}));
  auto a = g.AddNode(Op("Pass@1.0.0"), "a");
  auto b = g.AddNode(Op("Pass@1.0.0"), "b");
  auto s1 = g.AddNode(Op("Sink@1.0.0"), "sink1");
  auto s2 = g.AddNode(Op("Sink@1.0.0"), "sink2");
  g.Connect(src.port("out"), a.port("in"), {.id = "entry"});
  g.Connect(a.port("out"), s1.port("in"));
  g.Connect(a.port("out"), b.port("in"));
  g.Connect(b.port("out"), s2.port("in"));
  auto s = f.Create(*g.Build(), exec);
  ASSERT_TRUE(s->Start().ok());
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(exec.RunOne());
  auto dry = s->DryRun(ge::Mutation().RemoveBranch("entry", {.nodes = {"a", "sink1"}}).Build());
  EXPECT_EQ(dry.status().code(), GE_STATUS_SHARED_DEPENDENCY);
  EXPECT_NE(dry.status().context_json().find("\"external_node\":\"b\""), std::string::npos)
      << dry.status().context_json();
  auto op = s->Apply(ge::Mutation().RemoveBranch("entry", {.nodes = {"a", "sink1"}}).Build());
  ASSERT_TRUE(op.ok());
  Drain(exec, *s);
  const auto rec = WaitOp(f.ops, *op);
  EXPECT_EQ(rec.state, ge::OperationState::kFailed);
  EXPECT_EQ(rec.result.code(), GE_STATUS_SHARED_DEPENDENCY);
  EXPECT_TRUE(rec.detail.as_object().contains("context"));
  EXPECT_EQ(s->topology_version(), 1U);  // old graph unchanged
  EXPECT_EQ(f.sinks["sink1"]->Seqs(), Iota(50));
  EXPECT_EQ(f.sinks["sink2"]->Seqs(), Iota(50));
}

TEST(SessionTest, RemoveBranchDrainsOnlyExclusivePath) {
  Fixture f;
  ge::ExecutorPool exec(3);
  ge::GraphBuilder g("branch");
  auto src = g.AddNode(Op("Tick@1.0.0"), "src");
  auto a = g.AddNode(Op("Pass@1.0.0"), "a");
  auto b = g.AddNode(Op("Pass@1.0.0"), "b");
  auto s1 = g.AddNode(Op("Sink@1.0.0"), "sink1");
  auto s2 = g.AddNode(Op("Sink@1.0.0"), "sink2");
  g.Connect(src.port("out"), a.port("in"), {.id = "to_a"});
  g.Connect(src.port("out"), b.port("in"), {.id = "to_b"});
  g.Connect(a.port("out"), s1.port("in"));
  g.Connect(b.port("out"), s2.port("in"));
  auto s = f.Create(*g.Build(), exec);
  ASSERT_TRUE(s->Start().ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  auto op = s->Apply(ge::Mutation().RemoveBranch("to_b", {.nodes = {"b", "sink2"}}).Build());
  ASSERT_TRUE(op.ok());
  const auto rec = WaitOp(f.ops, *op);
  EXPECT_EQ(rec.state, ge::OperationState::kSucceeded) << rec.result.ToString();
  EXPECT_TRUE(f.passes["b"]->closed);
  EXPECT_TRUE(f.sinks["sink2"]->flushed);  // drained, not fast
  EXPECT_FALSE(f.sinks["sink2"]->fast_close);
  const std::size_t s2_count = f.sinks["sink2"]->Seqs().size();
  const std::size_t s1_at = f.sinks["sink1"]->Seqs().size();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_GT(f.sinks["sink1"]->Seqs().size(), s1_at);  // common path keeps flowing
  EXPECT_EQ(f.sinks["sink2"]->Seqs().size(), s2_count);
  EXPECT_EQ(s->current_topology()->FindNode("b"), nullptr);
  auto stop = s->Stop(true);
  ASSERT_TRUE(stop.ok());
  WaitOp(f.ops, *stop);
  const auto seqs = f.sinks["sink1"]->Seqs();
  EXPECT_EQ(seqs, Iota(static_cast<std::int64_t>(seqs.size())));  // MUT-3: unaffected path lossless
}

TEST(SessionTest, RemoveBranchFastDropsQueuedPackets) {
  Fixture f;
  ge::ExecutorPool exec(0);
  ge::GraphBuilder g("fast");
  auto src = g.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(100)}}));
  auto a = g.AddNode(Op("Pass@1.0.0"), "a");
  auto b = g.AddNode(Op("Pass@1.0.0"), "b");
  auto s1 = g.AddNode(Op("Sink@1.0.0"), "sink1");
  auto s2 = g.AddNode(Op("Sink@1.0.0"), "sink2");
  g.Connect(src.port("out"), a.port("in"));
  g.Connect(src.port("out"), b.port("in"), {.id = "to_b", .queue = {.capacity = 128}});
  g.Connect(a.port("out"), s1.port("in"));
  g.Connect(b.port("out"), s2.port("in"), {.queue = {.capacity = 128}});
  auto s = f.Create(*g.Build(), exec);
  ASSERT_TRUE(s->Start().ok());
  for (int i = 0; i < 30; ++i) ASSERT_TRUE(exec.RunOne());
  auto op = s->Apply(ge::Mutation().SetRemovePolicy(ge::RemovePolicy::kFast).RemoveBranch("to_b", {.nodes = {"b", "sink2"}}).Build());
  ASSERT_TRUE(op.ok());
  ASSERT_TRUE(s->PumpMutations());
  const std::size_t s2_at_publish = f.sinks["sink2"]->Seqs().size();
  EXPECT_EQ(s->current_topology()->FindEdge("to_b"), nullptr);
  Drain(exec, *s);
  const auto rec = WaitOp(f.ops, *op);
  EXPECT_EQ(rec.state, ge::OperationState::kSucceeded) << rec.result.ToString();
  EXPECT_TRUE(f.sinks["sink2"]->closed);
  EXPECT_TRUE(f.sinks["sink2"]->fast_close);
  EXPECT_FALSE(f.sinks["sink2"]->flushed);
  EXPECT_EQ(f.sinks["sink2"]->Seqs().size(), s2_at_publish);  // queued packets dropped
  EXPECT_EQ(f.sinks["sink1"]->Seqs(), Iota(100));
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

TEST(SessionTest, ReplaceNodeMigratesParametersAndRetiresOld) {
  Fixture f;
  ge::ExecutorPool exec(0);
  auto s = f.Create(Linear(150), exec);
  ASSERT_TRUE(s->Start().ok());
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(exec.RunOne());
  ASSERT_TRUE(s->SetParameters("p0", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(9)}})).ok());
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(exec.RunOne());
  PassThrough* old_p0 = f.passes["p0"];
  auto op = s->Apply(ge::Mutation().ReplaceNode("p0", Op("Pass@2.0.0")).Build());
  ASSERT_TRUE(op.ok());
  Drain(exec, *s);
  const auto rec = WaitOp(f.ops, *op);
  EXPECT_EQ(rec.state, ge::OperationState::kSucceeded) << rec.result.ToString();
  ASSERT_NE(f.passes.find("p0@2"), f.passes.end());
  PassThrough* new_p0 = f.passes["p0@2"];
  EXPECT_TRUE(new_p0->opened);
  EXPECT_TRUE(old_p0->flushed);
  EXPECT_TRUE(old_p0->closed);
  EXPECT_EQ(s->current_topology()->FindNode("p0")->operator_key(), Op("Pass@2.0.0"));
  // Migrated live value (9), not the spec default (0).
  ASSERT_FALSE(new_p0->Records().empty());
  for (const auto& r : new_p0->Records()) EXPECT_EQ(r.gain, 9);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(150));
  EXPECT_EQ(old_p0->Records().size() + new_p0->Records().size(), 150U);
}

TEST(SessionTest, PrepareFailureLeavesOldGraph) {
  Fixture f;
  ge::ExecutorPool exec(0);
  auto s = f.Create(Linear(100), exec);
  ASSERT_TRUE(s->Start().ok());
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(exec.RunOne());
  const auto snapshot_before = s->Snapshot().Serialize();
  // Warm-up failure (A7).
  auto op1 = s->Apply(ge::Mutation().InsertChain("e1", {Node("w", "BadOpen@1.0.0")}).Build());
  // Validation failure (A4): unknown operator.
  auto op2 = s->Apply(ge::Mutation().InsertChain("e1", {Node("x", "Nope@1.0.0")}).Build());
  // Version conflict.
  auto op3 = s->Apply(ge::Mutation().SetBaseVersion(5).RemoveNode("p0").Build());
  ASSERT_TRUE(op1.ok() && op2.ok() && op3.ok());
  while (s->PumpMutations()) {
  }
  EXPECT_EQ(WaitOp(f.ops, *op1).result.code(), GE_STATUS_NODE_WARMUP_FAILED);
  EXPECT_EQ(WaitOp(f.ops, *op2).result.code(), GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(WaitOp(f.ops, *op3).result.code(), GE_STATUS_VERSION_CONFLICT);
  EXPECT_EQ(s->topology_version(), 1U);
  EXPECT_EQ(s->current_topology()->FindNode("w"), nullptr);
  Drain(exec, *s);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(100));
  EXPECT_EQ(s->scheduler().pending_retirements(), 0U);
  (void)snapshot_before;
}

TEST(SessionTest, QueueMergesCompatibleAndSerialisesRest) {
  Fixture f;
  ge::ExecutorPool exec(0);
  ge::GraphBuilder g("merge");
  auto src = g.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(100)}}));
  auto a = g.AddNode(Op("Pass@1.0.0"), "a");
  auto b = g.AddNode(Op("Pass@1.0.0"), "b");
  auto s1 = g.AddNode(Op("Sink@1.0.0"), "sink1");
  auto s2 = g.AddNode(Op("Sink@1.0.0"), "sink2");
  g.Connect(src.port("out"), a.port("in"));
  g.Connect(src.port("out"), b.port("in"));
  g.Connect(a.port("out"), s1.port("in"), {.id = "a_out"});
  g.Connect(b.port("out"), s2.port("in"), {.id = "b_out"});
  auto s = f.Create(*g.Build(), exec);
  ASSERT_TRUE(s->Start().ok());
  for (int i = 0; i < 4; ++i) ASSERT_TRUE(exec.RunOne());
  // Two disjoint inserts merge into one version; the third touches a_out's
  // successor chain and thus a different edge -> also disjoint; the fourth
  // conflicts with the first (same node "a" region) via remove_policy change.
  auto o1 = s->Apply(ge::Mutation().InsertChain("a_out", {Node("a1", "Pass@1.0.0")}).Build());
  auto o2 = s->Apply(ge::Mutation().InsertChain("b_out", {Node("b1", "Pass@1.0.0")}).Build());
  auto o3 = s->Apply(ge::Mutation().SetRemovePolicy(ge::RemovePolicy::kFast).SetNodeOptions("a", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(3)}})).Build());
  ASSERT_TRUE(o1.ok() && o2.ok() && o3.ok());
  ASSERT_TRUE(s->PumpMutations());
  EXPECT_EQ(s->topology_version(), 2U);
  EXPECT_NE(s->current_topology()->FindNode("a1"), nullptr);
  EXPECT_NE(s->current_topology()->FindNode("b1"), nullptr);
  EXPECT_EQ(f.ops.Get(*o3)->state, ge::OperationState::kAccepted);  // not merged (policy differs)
  ASSERT_TRUE(s->PumpMutations());
  EXPECT_FALSE(s->PumpMutations());
  Drain(exec, *s);
  const auto r1 = WaitOp(f.ops, *o1);
  const auto r2 = WaitOp(f.ops, *o2);
  const auto r3 = WaitOp(f.ops, *o3);
  EXPECT_EQ(r1.state, ge::OperationState::kSucceeded);
  EXPECT_EQ(r2.state, ge::OperationState::kSucceeded);
  EXPECT_EQ(r3.state, ge::OperationState::kSucceeded) << r3.result.ToString();
  EXPECT_EQ(r1.topology_version, 2U);
  EXPECT_EQ(r2.topology_version, 2U);  // shared Vn+1
  EXPECT_EQ(r3.topology_version, 3U);
  EXPECT_EQ(f.sinks["sink1"]->Seqs(), Iota(100));
  EXPECT_EQ(f.sinks["sink2"]->Seqs(), Iota(100));
  EXPECT_EQ(f.passes["a"]->gain_seen.load(), 3);
}

TEST(SessionTest, AddRemoveNodeAndEdgeDirectly) {
  Fixture f;
  ge::ExecutorPool exec(0);
  auto s = f.Create(Linear(120), exec);
  ASSERT_TRUE(s->Start().ok());
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(exec.RunOne());
  // Add a second sink fed from p0 (fan-out grows at runtime).
  auto o1 = s->Apply(ge::Mutation().AddNode(Node("sink2", "Sink@1.0.0")).AddEdge({"p0", "out"}, {"sink2", "in"}, {.id = "e2"}).Build());
  ASSERT_TRUE(o1.ok());
  ASSERT_TRUE(s->PumpMutations());
  const std::size_t sink_at = f.sinks["sink"]->Seqs().size();
  for (int i = 0; i < 30; ++i) (void)exec.RunOne();
  // Then take it away again (drain) via RemoveEdge + RemoveNode in one patch.
  auto o2 = s->Apply(ge::Mutation().RemoveEdge("e2").RemoveNode("sink2").Build());
  ASSERT_TRUE(o2.ok());
  Drain(exec, *s);
  EXPECT_EQ(WaitOp(f.ops, *o1).state, ge::OperationState::kSucceeded);
  const auto r2 = WaitOp(f.ops, *o2);
  EXPECT_EQ(r2.state, ge::OperationState::kSucceeded) << r2.result.ToString();
  EXPECT_EQ(s->topology_version(), 3U);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(120));
  const auto s2 = f.sinks["sink2"]->Seqs();
  EXPECT_FALSE(s2.empty());
  EXPECT_GT(s2.front(), static_cast<ge::PacketSeq>(sink_at));  // only packets after the add
  EXPECT_TRUE(std::is_sorted(s2.begin(), s2.end()));
  EXPECT_TRUE(f.sinks["sink2"]->flushed);
  EXPECT_TRUE(f.sinks["sink2"]->closed);
  EXPECT_LT(s2.size(), 120U);
}

TEST(SessionTest, DrainTimeoutUpgradesToFast) {
  Fixture f;
  ge::ExecutorPool exec(2);
  ge::GraphBuilder g("slow");
  auto src = g.AddNode(Op("Tick@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"period_us", ge::JsonValue(10)}}));
  auto a = g.AddNode(Op("Pass@1.0.0"), "a");
  // Stuck branch: the gate never opens on its own, so the drain cannot
  // complete and the deadline must fire. (A "slow" node with a per-packet
  // sleep was a timing guess: under a starved executor it drained within
  // the 30ms just often enough to flake.)
  auto slow = g.AddNode(Op("Gate@1.0.0"), "slow");
  auto s1 = g.AddNode(Op("Sink@1.0.0"), "sink1");
  auto s2 = g.AddNode(Op("Sink@1.0.0"), "sink2");
  g.Connect(src.port("out"), a.port("in"));
  g.Connect(src.port("out"), slow.port("in"), {.id = "to_slow", .queue = {.capacity = 64, .policy = ge::DropPolicy::kDropOldest}});
  g.Connect(a.port("out"), s1.port("in"));
  g.Connect(slow.port("out"), s2.port("in"));
  std::atomic<int> timeouts{0};
  auto s = f.Create(*g.Build(), exec, {.drain_timeout = std::chrono::milliseconds(30)},
                    {.on_drain_timeout = [&](ge::TopologyVersion) { ++timeouts; }});
  ASSERT_TRUE(s->Start().ok());
  Gate* gate = f.gates.at("slow");
  for (int i = 0; i < 2000 && gate->entered.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_GT(gate->entered.load(), 0);  // the branch is live and stuck in Process
  auto op = s->Apply(ge::Mutation().RemoveBranch("to_slow", {.nodes = {"slow", "sink2"}}).Build());
  ASSERT_TRUE(op.ok());
  // Watchdog role: tick until the deadline upgrades the retire to fast.
  // The engine never interrupts a running Process call (12 §7.7): the
  // cancelled node closes when its call returns, so open the gate only
  // after the timeout fired and keep ticking until the operation ends.
  for (int i = 0; i < 5000 && timeouts.load() == 0; ++i) {
    s->Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_EQ(timeouts.load(), 1);
  gate->Release();
  ge::OperationRecord rec;
  for (int i = 0; i < 5000; ++i) {
    s->Tick();
    if (auto r = f.ops.Get(*op); r && r->terminal()) {
      rec = *r;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(rec.state, ge::OperationState::kSucceeded) << rec.result.ToString();
  EXPECT_EQ(timeouts.load(), 1);
  EXPECT_TRUE(rec.detail.as_object().contains("drain_timeout_upgraded_to_fast"));
  // AUD-1: the applied diff and phase costs travel with the record.
  const auto& diff = rec.detail.as_object().at("diff").as_object();
  EXPECT_EQ(diff.at("nodes").as_object().at("slow").GetString("change"), "removed");
  EXPECT_EQ(diff.at("edges").as_object().at("to_slow").GetString("change"), "removed");
  EXPECT_TRUE(rec.detail.as_object().contains("prepare_ms"));
  EXPECT_TRUE(rec.detail.as_object().contains("retire_ms"));
  EXPECT_TRUE(gate->closed);
  EXPECT_TRUE(gate->fast_close);
  EXPECT_TRUE(f.sinks["sink2"]->closed);
  auto stop = s->Stop(true);
  ASSERT_TRUE(stop.ok());
  WaitOp(f.ops, *stop);
}

TEST(SessionTest, SnapshotAndOperationQuery) {
  Fixture f;
  ge::ExecutorPool exec(0);
  auto s = f.Create(Linear(10), exec, {.id = 42});
  ASSERT_TRUE(s->Start().ok());
  const ge::JsonValue snap = s->Snapshot();
  EXPECT_EQ(snap.GetInteger("session_id"), 42);
  EXPECT_EQ(snap.GetString("state"), "running");
  EXPECT_EQ(snap.GetInteger("topology_version"), 1);
  EXPECT_EQ(snap.as_object().at("nodes").as_array().size(), 3U);
  EXPECT_EQ(snap.as_object().at("edges").as_array().size(), 2U);
  EXPECT_TRUE(snap.as_object().at("edges").as_array()[0].as_object().contains("contract"));
  auto op = s->Apply(ge::Mutation().SetNodeOptions("p0", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(2)}})).Build());
  ASSERT_TRUE(op.ok());
  const auto accepted = f.ops.Get(*op);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted->state, ge::OperationState::kAccepted);
  EXPECT_EQ(accepted->kind, "mutation.apply");
  EXPECT_TRUE(accepted->ToJson().as_object().contains("detail"));
  Drain(exec, *s);
  const auto done = WaitOp(f.ops, *op);
  EXPECT_EQ(done.state, ge::OperationState::kSucceeded) << done.result.ToString();
  EXPECT_EQ(done.ToJson().GetString("state"), "succeeded");
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(10));
}

}  // namespace
