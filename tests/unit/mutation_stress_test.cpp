// Dynamic-graph stability: mutation storms, interleavings and failure mixes
// on a running session (SRS §4.1 "每分钟一次增删 24h", MUT-2/3/4/5, GM-7).
// Every case ends with the same invariants: unaffected sinks are lossless
// and ordered, no retirement is pending, and the buffer pool is empty.
#include <ge/cpp/session.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <numeric>
#include <random>
#include <thread>

#include "test_operators.h"

namespace {

using namespace ge::test;

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

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
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }

 private:
  std::shared_ptr<ge::HostBufferPool> pool_;
  std::chrono::microseconds period_;
  std::int64_t next_ = 0;
};

struct Fixture {
  std::shared_ptr<ge::HostBufferPool> pool = ge::HostBufferPool::Create();
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  ge::OperationRegistry ops;
  std::mutex mutex;
  std::map<std::string, PassThrough*> passes;
  std::map<std::string, Collector*> sinks;
  std::atomic<int> created_passes{0}, created_sinks{0};

  Fixture() {
    factory.Register(Desc("Src@1.0.0", {}, {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)}, true, 1),
                     [this](const ge::OperatorCreateArgs& a) {
                       const auto n = a.options.GetInteger("count").value_or(10);
                       return Keep(keep, std::make_shared<CountingSource>(n, pool));
                     });
    factory.Register(Desc("Tick@1.0.0", {}, {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)}, true, 1),
                     [this](const ge::OperatorCreateArgs& a) {
                       const auto us = a.options.GetInteger("period_us").value_or(20);
                       return Keep(keep, std::make_shared<TickingSource>(pool, std::chrono::microseconds(us)));
                     });
    auto pass_desc = Desc("Pass@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)}, false, 1);
    pass_desc.parameters.migratable = {"gain"};
    auto make_pass = [this](const ge::OperatorCreateArgs& a) {
      const auto us = a.options.GetInteger("delay_us").value_or(0);
      auto p = std::make_shared<PassThrough>(std::chrono::microseconds(us));
      std::lock_guard lock(mutex);
      passes[a.external_id] = p.get();
      ++created_passes;
      return Keep(keep, p);
    };
    factory.Register(pass_desc, make_pass);
    auto pass2 = pass_desc;
    pass2.op = Op("Pass@2.0.0");
    factory.Register(pass2, make_pass);
    factory.Register(Desc("Sink@1.0.0", {BytesPort("in", ge::PortDirection::kInput)}, {}),
                     [this](const ge::OperatorCreateArgs& a) {
                       auto c = std::make_shared<Collector>();
                       std::lock_guard lock(mutex);
                       sinks[a.external_id] = c.get();
                       ++created_sinks;
                       return Keep(keep, c);
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
  PassThrough* Pass(const std::string& id) {
    std::lock_guard lock(mutex);
    return passes.at(id);
  }
  Collector* Sink(const std::string& id) {
    std::lock_guard lock(mutex);
    return sinks.at(id);
  }
};

ge::NodeSpec Node(const std::string& id, const char* op, ge::JsonValue options = ge::JsonValue(ge::JsonObject{})) {
  ge::NodeSpec n;
  n.id = id;
  n.op = Op(op);
  n.options = std::move(options);
  return n;
}

// src -> p0 -> sink; Tick source when count < 0.
ge::GraphSpec Linear(std::int64_t count, std::uint32_t capacity = 64) {
  ge::GraphBuilder b("linear");
  auto src = count < 0 ? b.AddNode(Op("Tick@1.0.0"), "src")
                       : b.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(count)}}));
  auto p0 = b.AddNode(Op("Pass@1.0.0"), "p0");
  auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  b.Connect(src.port("out"), p0.port("in"), {.id = "e0", .queue = {.capacity = capacity}});
  b.Connect(p0.port("out"), sink.port("in"), {.id = "e1", .queue = {.capacity = capacity}});
  return *b.Build();
}

// A transcode-like branch hung off src.out: src -> pass_<n> -> sink_<n>.
ge::MutationPatch AddBranch(const std::string& n, std::uint32_t capacity = 64) {
  return ge::Mutation()
      .AddNode(Node("pass_" + n, "Pass@1.0.0"))
      .AddNode(Node("sink_" + n, "Sink@1.0.0"))
      .AddEdge({"src", "out"}, {"pass_" + n, "in"}, {.id = "to_" + n, .queue = {.capacity = capacity}})
      .AddEdge({"pass_" + n, "out"}, {"sink_" + n, "in"}, {.queue = {.capacity = capacity}})
      .Build();
}

ge::MutationPatch RemoveBranch(const std::string& n, ge::RemovePolicy policy) {
  return ge::Mutation().SetRemovePolicy(policy).RemoveBranch("to_" + n, {.nodes = {"pass_" + n, "sink_" + n}}).Build();
}

std::vector<ge::PacketSeq> Iota(std::size_t n) {
  std::vector<ge::PacketSeq> v(n);
  std::iota(v.begin(), v.end(), 1U);
  return v;
}

void ExpectLossless(const Collector& c) {
  const auto seqs = c.Seqs();
  ASSERT_FALSE(seqs.empty());
  for (std::size_t i = 0; i < seqs.size(); ++i) {
    if (seqs[i] == i + 1) continue;
    std::string ctx;
    for (std::size_t j = (i > 4 ? i - 4 : 0); j < std::min(seqs.size(), i + 5); ++j) ctx += std::to_string(seqs[j]) + " ";
    FAIL() << "first mismatch at index " << i << " (expected " << i + 1 << "): " << ctx << " total=" << seqs.size();
  }
}

// Inline executor: an operation is terminal only once the retire of the old
// version completed, which needs executor turns. Settle before waiting.
void Drain(ge::ExecutorPool& exec, ge::Session& s) {
  for (int i = 0; i < 200000; ++i) {
    bool did = s.PumpMutations();
    did = exec.RunPending() || did;
    s.Tick();
    if (!did && !exec.RunPending()) return;
  }
}

ge::OperationRecord WaitOp(ge::OperationRegistry& ops, ge::OperationId id, int ms = 10000) {
  auto r = ops.Wait(id, std::chrono::milliseconds(ms));
  EXPECT_TRUE(r.has_value()) << "operation " << id << " did not finish";
  return r.value_or(ge::OperationRecord{});
}

void ExpectSucceeded(ge::OperationRegistry& ops, ge::OperationId id) {
  const auto rec = WaitOp(ops, id);
  EXPECT_EQ(rec.state, ge::OperationState::kSucceeded) << rec.result.ToString() << " " << rec.detail.Serialize();
}

void StopAndCheck(Fixture& f, ge::Session& s, bool fast = false) {
  auto stop = s.Stop(fast);
  ASSERT_TRUE(stop.ok()) << stop.status().ToString();
  WaitOp(f.ops, *stop);
  EXPECT_EQ(s.state(), ge::SessionState::kStopped);
  EXPECT_EQ(s.scheduler().pending_retirements(), 0U);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

// ---------------------------------------------------------------------------
// 1. Branch add/remove storm on a live pipeline (threaded, drain/fast mix,
//    same ids reused every round).
// ---------------------------------------------------------------------------

TEST(MutationStressTest, BranchAddRemoveStormReusingIds) {
  Fixture f;
  ge::ExecutorPool exec(4);
  auto s = f.Create(Linear(-1), exec);
  ASSERT_TRUE(s->Start().ok());
  constexpr int kRounds = 30;
  for (int i = 0; i < kRounds; ++i) {
    const std::string n = "b" + std::to_string(i % 3);  // three ids cycled: reuse right after removal
    auto add = s->Apply(AddBranch(n));
    ASSERT_TRUE(add.ok()) << add.status().ToString();
    ExpectSucceeded(f.ops, *add);
    Collector* sink = f.Sink("sink_" + n);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const ge::RemovePolicy policy = (i % 2) ? ge::RemovePolicy::kFast : ge::RemovePolicy::kDrain;
    auto rm = s->Apply(RemoveBranch(n, policy));
    ASSERT_TRUE(rm.ok());
    ExpectSucceeded(f.ops, *rm);
    EXPECT_TRUE(sink->closed) << i;
    EXPECT_EQ(sink->fast_close.load(), policy == ge::RemovePolicy::kFast) << i;
    EXPECT_EQ(s->current_topology()->FindNode("pass_" + n), nullptr);
    EXPECT_EQ(s->current_topology()->FindEdge("to_" + n), nullptr);
    const auto seqs = sink->Seqs();
    EXPECT_TRUE(std::is_sorted(seqs.begin(), seqs.end())) << i;
  }
  EXPECT_EQ(s->topology_version(), 1U + 2U * kRounds);
  EXPECT_EQ(f.created_sinks.load(), kRounds + 1);
  StopAndCheck(f, *s);
  ExpectLossless(*f.Sink("sink"));
  // Every retired branch operator was closed exactly once and dropped.
  EXPECT_EQ(s->current_topology()->nodes().size(), 3U);
}

// ---------------------------------------------------------------------------
// 2. Insert-chain / remove-with-bypass ping-pong under randomised executor
//    interleaving (inline, deterministic seed).
// ---------------------------------------------------------------------------

TEST(MutationStressTest, InsertBypassPingPongDeterministic) {
  Fixture f;
  ge::ExecutorPool exec(0);
  constexpr std::int64_t kCount = 3000;
  auto s = f.Create(Linear(kCount, 8), exec);
  ASSERT_TRUE(s->Start().ok());
  std::mt19937 rng(20260919);
  std::vector<ge::OperationId> ids;
  int inserted = 0;
  // The bypass edge is auto-named after its ports; the next insert targets it.
  std::string edge = "e0";
  for (int round = 0; round < 40 && f.Sink("sink")->Seqs().size() < static_cast<std::size_t>(kCount) - 200; ++round) {
    for (int k = std::uniform_int_distribution<int>(1, 25)(rng); k > 0; --k) (void)exec.RunOne();
    const std::string a = "a" + std::to_string(round), b = "b" + std::to_string(round);
    auto ins = s->Apply(ge::Mutation().InsertChain(edge, {Node(a, "Pass@1.0.0"), Node(b, "Pass@1.0.0")}).Build());
    ASSERT_TRUE(ins.ok());
    ids.push_back(*ins);
    ++inserted;
    // Randomised interleaving of executor turns and the publish itself.
    for (int k = std::uniform_int_distribution<int>(0, 25)(rng); k > 0; --k) (void)exec.RunOne();
    ASSERT_TRUE(s->PumpMutations());
    for (int k = std::uniform_int_distribution<int>(0, 25)(rng); k > 0; --k) {
      (void)exec.RunOne();
      s->Tick();
    }
    auto rm = s->Apply(ge::Mutation()
                           .SetRemovePolicy((round % 2) ? ge::RemovePolicy::kFast : ge::RemovePolicy::kDrain)
                           .RemoveChain({a, b}, {.bypass = true})
                           .Build());
    ASSERT_TRUE(rm.ok());
    ids.push_back(*rm);
    for (int k = std::uniform_int_distribution<int>(0, 25)(rng); k > 0; --k) (void)exec.RunOne();
    ASSERT_TRUE(s->PumpMutations());
    edge = "src.out->p0.in";
    for (int k = std::uniform_int_distribution<int>(0, 25)(rng); k > 0; --k) {
      (void)exec.RunOne();
      s->Tick();
    }
  }
  Drain(exec, *s);
  for (const ge::OperationId id : ids) ExpectSucceeded(f.ops, id);
  EXPECT_EQ(s->state(), ge::SessionState::kStopped);
  EXPECT_EQ(s->topology_version(), 1U + 2U * static_cast<unsigned>(inserted));
  EXPECT_EQ(s->current_topology()->nodes().size(), 3U);
  EXPECT_NE(s->current_topology()->FindEdge("src.out->p0.in"), nullptr);
  const auto seqs = f.Sink("sink")->Seqs();
  EXPECT_TRUE(std::is_sorted(seqs.begin(), seqs.end()));
  EXPECT_EQ(std::adjacent_find(seqs.begin(), seqs.end()), seqs.end());  // never duplicated
  EXPECT_EQ(seqs.back(), static_cast<ge::PacketSeq>(kCount));
  // Fast rounds may drop the chain's queued packets (MUT-5); drain rounds
  // must not. Count what went missing and bound it by the fast-round budget.
  const std::size_t missing = static_cast<std::size_t>(kCount) - seqs.size();
  EXPECT_LE(missing, static_cast<std::size_t>(inserted / 2 + 1) * 3U * 8U);
  for (int r = 0; r < inserted; ++r) {
    EXPECT_TRUE(f.Pass("a" + std::to_string(r))->closed) << r;
    EXPECT_TRUE(f.Pass("b" + std::to_string(r))->closed) << r;
  }
  EXPECT_EQ(s->scheduler().pending_retirements(), 0U);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

TEST(MutationStressTest, InsertBypassDrainOnlyIsLossless) {
  Fixture f;
  ge::ExecutorPool exec(0);
  constexpr std::int64_t kCount = 2000;
  auto s = f.Create(Linear(kCount, 8), exec);
  ASSERT_TRUE(s->Start().ok());
  std::mt19937 rng(7);
  int rounds = 0;
  std::string edge = "e0";
  while (f.Sink("sink")->Seqs().size() < static_cast<std::size_t>(kCount) - 100 && rounds < 60) {
    const std::string a = "a" + std::to_string(rounds);
    auto ins = s->Apply(ge::Mutation().InsertChain(edge, {Node(a, "Pass@1.0.0")}).Build());
    ASSERT_TRUE(ins.ok());
    for (int k = std::uniform_int_distribution<int>(0, 30)(rng); k > 0; --k) (void)exec.RunOne();
    ASSERT_TRUE(s->PumpMutations());
    for (int k = std::uniform_int_distribution<int>(0, 30)(rng); k > 0; --k) {
      (void)exec.RunOne();
      s->Tick();
    }
    auto rm = s->Apply(ge::Mutation().RemoveChain({a}, {.bypass = true}).Build());
    ASSERT_TRUE(rm.ok());
    for (int k = std::uniform_int_distribution<int>(0, 30)(rng); k > 0; --k) (void)exec.RunOne();
    ASSERT_TRUE(s->PumpMutations());
    edge = "src.out->p0.in";
    for (int k = std::uniform_int_distribution<int>(0, 30)(rng); k > 0; --k) {
      (void)exec.RunOne();
      s->Tick();
    }
    ++rounds;
  }
  Drain(exec, *s);
  EXPECT_GT(rounds, 5);
  EXPECT_EQ(f.Sink("sink")->Seqs(), Iota(static_cast<std::size_t>(kCount)));
  EXPECT_EQ(s->topology_version(), 1U + 2U * static_cast<unsigned>(rounds));
  EXPECT_EQ(s->scheduler().pending_retirements(), 0U);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

// ---------------------------------------------------------------------------
// 3. Several controller threads submitting mutations concurrently (MUT-4:
//    per-session serial queue; nothing is lost or half-applied).
// ---------------------------------------------------------------------------

TEST(MutationStressTest, ConcurrentSubmittersSerialisedWithoutLoss) {
  Fixture f;
  ge::ExecutorPool exec(4);
  // Declared before the session: the coordinator thread may still deliver
  // on_topology_published until ~Session joins it.
  std::atomic<int> published{0};
  auto s = f.Create(Linear(-1), exec);
  ASSERT_TRUE(s->Start().ok());
  s->events().on_topology_published = [&](ge::TopologyVersion) { ++published; };
  constexpr int kThreads = 4, kRounds = 8;
  std::vector<std::thread> threads;
  std::vector<std::vector<ge::OperationId>> ids(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kRounds; ++i) {
        const std::string n = "t" + std::to_string(t) + "_" + std::to_string(i);
        auto add = s->Apply(AddBranch(n));
        ASSERT_TRUE(add.ok());
        ids[t].push_back(*add);
        WaitOp(f.ops, *add);
        auto rm = s->Apply(RemoveBranch(n, (i % 2) ? ge::RemovePolicy::kFast : ge::RemovePolicy::kDrain));
        ASSERT_TRUE(rm.ok());
        ids[t].push_back(*rm);
      }
    });
  }
  for (auto& th : threads) th.join();
  for (const auto& v : ids)
    for (const ge::OperationId id : v) ExpectSucceeded(f.ops, id);
  EXPECT_EQ(s->current_topology()->nodes().size(), 3U);
  EXPECT_EQ(f.created_sinks.load(), kThreads * kRounds + 1);
  for (int t = 0; t < kThreads; ++t)
    for (int i = 0; i < kRounds; ++i) EXPECT_TRUE(f.Sink("sink_t" + std::to_string(t) + "_" + std::to_string(i))->closed);
  // Merged batches: fewer publishes than operations, never more.
  EXPECT_LE(published.load(), 2 * kThreads * kRounds);
  EXPECT_GT(published.load(), 0);
  StopAndCheck(f, *s);
  ExpectLossless(*f.Sink("sink"));
}

// ---------------------------------------------------------------------------
// 4. Mutations interleaved with parameter updates on a kept node (PAR-3/5).
// ---------------------------------------------------------------------------

TEST(MutationStressTest, ParameterUpdatesAcrossTopologySwaps) {
  Fixture f;
  ge::ExecutorPool exec(0);
  constexpr std::int64_t kCount = 1500;
  auto s = f.Create(Linear(kCount), exec);
  ASSERT_TRUE(s->Start().ok());
  std::int64_t gain = 0;
  for (int round = 0; round < 25; ++round) {
    for (int k = 0; k < 7; ++k) (void)exec.RunOne();
    ASSERT_TRUE(s->SetParameters("p0", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(++gain)}})).ok());
    const std::string n = "x" + std::to_string(round);
    auto ins = s->Apply(ge::Mutation().InsertChain("e1", {Node(n, "Pass@1.0.0")}).Build());
    ASSERT_TRUE(ins.ok());
    ASSERT_TRUE(s->SetParameters("p0", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(++gain)}})).ok());
    for (int k = 0; k < 5; ++k) {
      (void)s->PumpMutations();
      (void)exec.RunOne();
    }
    auto rm = s->Apply(ge::Mutation().RemoveChain({n}, {.bypass = true}).Build());
    ASSERT_TRUE(rm.ok());
    for (int k = 0; k < 5; ++k) {
      (void)s->PumpMutations();
      (void)exec.RunOne();
    }
  }
  Drain(exec, *s);
  EXPECT_EQ(f.Sink("sink")->Seqs(), Iota(static_cast<std::size_t>(kCount)));
  EXPECT_EQ(f.Pass("p0")->gain_seen.load(), gain);
  // Parameter versions are monotone along the packet stream (PAR-4).
  const auto recs = f.Pass("p0")->Records();
  ASSERT_EQ(recs.size(), static_cast<std::size_t>(kCount));
  for (std::size_t i = 1; i < recs.size(); ++i) {
    EXPECT_LE(recs[i - 1].gain, recs[i].gain) << i;
    EXPECT_LE(recs[i - 1].topology_version, recs[i].topology_version) << i;
  }
  EXPECT_EQ(s->scheduler().pending_retirements(), 0U);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

// ---------------------------------------------------------------------------
// 5. Failing mutations mixed with valid ones: every failure leaves the graph
//    untouched and leaks nothing (MUT-2).
// ---------------------------------------------------------------------------

TEST(MutationStressTest, FailureMixLeavesGraphIntact) {
  Fixture f;
  ge::ExecutorPool exec(0);
  constexpr std::int64_t kCount = 1200;
  auto s = f.Create(Linear(kCount), exec);
  ASSERT_TRUE(s->Start().ok());
  ge::TopologyVersion expected = 1;
  int good = 0;
  std::string edge = "e1";  // bypass re-creates it under the auto name
  for (int round = 0; round < 20; ++round) {
    for (int k = 0; k < 6; ++k) (void)exec.RunOne();
    std::vector<std::pair<ge::OperationId, int>> failing;  // id, expected code
    switch (round % 4) {
      case 0: {  // warm-up failure
        auto op = s->Apply(ge::Mutation().InsertChain(edge, {Node("bad" + std::to_string(round), "BadOpen@1.0.0")}).Build());
        ASSERT_TRUE(op.ok());
        failing.emplace_back(*op, GE_STATUS_NODE_WARMUP_FAILED);
        break;
      }
      case 1: {  // unknown operator
        auto op = s->Apply(ge::Mutation().InsertChain(edge, {Node("nope" + std::to_string(round), "Nope@1.0.0")}).Build());
        ASSERT_TRUE(op.ok());
        failing.emplace_back(*op, GE_STATUS_GRAPH_INVALID);
        break;
      }
      case 2: {  // stale base version
        auto op = s->Apply(ge::Mutation().SetBaseVersion(expected + 7).RemoveNode("p0").Build());
        ASSERT_TRUE(op.ok());
        failing.emplace_back(*op, GE_STATUS_VERSION_CONFLICT);
        break;
      }
      default: {  // removing a node that does not exist
        auto op = s->Apply(ge::Mutation().RemoveNode("ghost" + std::to_string(round)).Build());
        ASSERT_TRUE(op.ok());
        failing.emplace_back(*op, GE_STATUS_NOT_FOUND);
        break;
      }
    }
    // A valid one right behind it (same queue turn or next).
    const std::string n = "g" + std::to_string(round);
    auto ins = s->Apply(ge::Mutation().SetBaseVersion(expected).InsertChain(edge, {Node(n, "Pass@1.0.0")}).Build());
    ASSERT_TRUE(ins.ok());
    while (s->PumpMutations()) {
    }
    for (int k = 0; k < 40 && !f.ops.Get(*ins)->terminal(); ++k) {
      (void)exec.RunOne();
      s->Tick();
    }
    for (const auto& [id, code] : failing) {
      const auto rec = WaitOp(f.ops, id);
      EXPECT_EQ(rec.state, ge::OperationState::kFailed) << round;
      EXPECT_EQ(rec.result.code(), code) << round << " " << rec.result.ToString();
    }
    ExpectSucceeded(f.ops, *ins);
    ++expected;
    ++good;
    EXPECT_EQ(s->topology_version(), expected);
    for (int k = 0; k < 6; ++k) (void)exec.RunOne();
    auto rm = s->Apply(ge::Mutation().RemoveChain({n}, {.bypass = true}).Build());
    ASSERT_TRUE(rm.ok());
    while (s->PumpMutations()) {
    }
    for (int k = 0; k < 40 && !f.ops.Get(*rm)->terminal(); ++k) {
      (void)exec.RunOne();
      s->Tick();
    }
    ExpectSucceeded(f.ops, *rm);
    ++expected;
    edge = "p0.out->sink.in";
  }
  Drain(exec, *s);
  EXPECT_EQ(good, 20);
  EXPECT_EQ(s->topology_version(), expected);
  EXPECT_EQ(s->current_topology()->nodes().size(), 3U);
  EXPECT_EQ(f.Sink("sink")->Seqs(), Iota(static_cast<std::size_t>(kCount)));
  EXPECT_EQ(s->scheduler().pending_retirements(), 0U);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

// ---------------------------------------------------------------------------
// 6. Pause / mutate / resume cycles (mutations are accepted while paused).
// ---------------------------------------------------------------------------

TEST(MutationStressTest, MutateWhilePausedThenResume) {
  Fixture f;
  ge::ExecutorPool exec(3);
  auto s = f.Create(Linear(-1), exec);
  ASSERT_TRUE(s->Start().ok());
  for (int round = 0; round < 10; ++round) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(s->Pause().ok());
    const std::string n = "p" + std::to_string(round);
    auto add = s->Apply(AddBranch(n));
    ASSERT_TRUE(add.ok());
    ExpectSucceeded(f.ops, *add);
    EXPECT_EQ(s->state(), ge::SessionState::kPaused);
    // Pause gates the source; in-flight packets still drain to the sink.
    // Wait until nothing is in flight before asserting the count stays put.
    ASSERT_TRUE(WaitDrained(*s->current_topology(), std::chrono::seconds(5))) << round;
    const std::size_t at_pause = f.Sink("sink")->Seqs().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    EXPECT_EQ(f.Sink("sink")->Seqs().size(), at_pause);  // still paused after the swap
    ASSERT_TRUE(s->Resume().ok());
    bool resumed = false;
    for (int i = 0; i < 500 && !resumed; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      resumed = f.Sink("sink")->Seqs().size() > at_pause && !f.Sink("sink_" + n)->Seqs().empty();
    }
    EXPECT_TRUE(resumed) << round;  // main path and new branch both receive after resume
    auto rm = s->Apply(RemoveBranch(n, ge::RemovePolicy::kDrain));
    ASSERT_TRUE(rm.ok());
    ExpectSucceeded(f.ops, *rm);
  }
  StopAndCheck(f, *s);
  ExpectLossless(*f.Sink("sink"));
}

// ---------------------------------------------------------------------------
// 7. Stop (drain and fast) with a queue full of pending mutations: no hang,
//    every operation reaches a terminal state, nothing leaks.
// ---------------------------------------------------------------------------

void StopWithPendingQueue(bool fast) {
  Fixture f;
  ge::ExecutorPool exec(3);
  auto s = f.Create(Linear(-1), exec);
  ASSERT_TRUE(s->Start().ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  std::vector<ge::OperationId> ids;
  for (int i = 0; i < 12; ++i) {
    auto add = s->Apply(AddBranch("q" + std::to_string(i)));
    ASSERT_TRUE(add.ok());
    ids.push_back(*add);
  }
  auto stop = s->Stop(fast);
  ASSERT_TRUE(stop.ok());
  const auto rec = WaitOp(f.ops, *stop);
  EXPECT_EQ(rec.state, ge::OperationState::kSucceeded) << rec.result.ToString();
  EXPECT_EQ(s->state(), ge::SessionState::kStopped);
  int succeeded = 0, cancelled = 0, failed = 0;
  for (const ge::OperationId id : ids) {
    const auto r = WaitOp(f.ops, id, 2000);
    EXPECT_TRUE(r.terminal()) << id;
    succeeded += r.state == ge::OperationState::kSucceeded;
    cancelled += r.state == ge::OperationState::kCancelled;
    failed += r.state == ge::OperationState::kFailed;
  }
  EXPECT_EQ(succeeded + cancelled + failed, 12);
  // Mutations after Stop are refused synchronously.
  EXPECT_FALSE(s->Apply(AddBranch("late")).ok());
  EXPECT_EQ(s->scheduler().pending_retirements(), 0U);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
  // Whatever was published got closed with the session.
  for (const auto& [id, sink] : f.sinks) EXPECT_TRUE(sink->closed) << id;
  if (!fast) ExpectLossless(*f.Sink("sink"));
}

TEST(MutationStressTest, DrainStopWithPendingMutations) { StopWithPendingQueue(false); }
TEST(MutationStressTest, FastStopWithPendingMutations) { StopWithPendingQueue(true); }

// ---------------------------------------------------------------------------
// 8. ReplaceNode storm on the kept middle node (MUT-1b), alternating
//    versions; parameters migrate every hop and nothing is lost.
// ---------------------------------------------------------------------------

TEST(MutationStressTest, ReplaceNodeStormMigratesAndCloses) {
  Fixture f;
  ge::ExecutorPool exec(0);
  constexpr std::int64_t kCount = 1000;
  auto s = f.Create(Linear(kCount), exec);
  ASSERT_TRUE(s->Start().ok());
  ASSERT_TRUE(s->SetParameters("p0", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(5)}})).ok());
  std::vector<PassThrough*> generations{f.Pass("p0")};
  for (int round = 0; round < 20; ++round) {
    for (int k = 0; k < 9; ++k) (void)exec.RunOne();
    const char* next = (round % 2 == 0) ? "Pass@2.0.0" : "Pass@1.0.0";
    auto op = s->Apply(ge::Mutation().ReplaceNode("p0", Op(next)).Build());
    ASSERT_TRUE(op.ok());
    ASSERT_TRUE(s->PumpMutations());
    // Old p0 drains on the executor; a few turns are enough, but do not run
    // the source dry (we want packets to cross the next swap too).
    for (int k = 0; k < 40 && !f.ops.Get(*op)->terminal(); ++k) {
      (void)exec.RunOne();
      s->Tick();
    }
    ExpectSucceeded(f.ops, *op);
    EXPECT_EQ(s->current_topology()->FindNode("p0")->operator_key(), Op(next)) << round;
    generations.push_back(f.Pass("p0"));
    ASSERT_NE(generations.back(), generations[generations.size() - 2]) << round;
  }
  Drain(exec, *s);
  EXPECT_EQ(f.Sink("sink")->Seqs(), Iota(static_cast<std::size_t>(kCount)));
  std::size_t total = 0;
  for (std::size_t g = 0; g < generations.size(); ++g) {
    EXPECT_TRUE(generations[g]->closed) << g;
    if (g + 1 < generations.size()) {
      EXPECT_TRUE(generations[g]->flushed) << g;  // drained, not fast
    }
    for (const auto& r : generations[g]->Records()) EXPECT_EQ(r.gain, 5) << g;  // migrated each hop
    total += generations[g]->Records().size();
  }
  EXPECT_EQ(total, static_cast<std::size_t>(kCount));
  EXPECT_EQ(s->scheduler().pending_retirements(), 0U);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

// ---------------------------------------------------------------------------
// 9. Drain/Fast removals of a backpressured slow branch (capacity 4, slow
//    consumer) must never touch the fast main path (MUT-3, FAN-4).
// ---------------------------------------------------------------------------

TEST(MutationStressTest, SlowBranchRemovalUnderBackpressure) {
  Fixture f;
  ge::ExecutorPool exec(3);
  std::atomic<int> timeouts{0};  // outlives the session (see test 3)
  auto s = f.Create(Linear(-1), exec, {.drain_timeout = std::chrono::milliseconds(200)});
  ASSERT_TRUE(s->Start().ok());
  s->events().on_drain_timeout = [&](ge::TopologyVersion) { ++timeouts; };
  // MUT-3: the untouched main path keeps its EdgeChannel objects across
  // every publish (no silent recreate + rebind of p0).
  const ge::EdgeChannel* e0 = s->current_topology()->FindEdge("e0");
  const ge::EdgeChannel* e1 = s->current_topology()->FindEdge("e1");
  // Capture the Session, not the unique_ptr: ~unique_ptr nulls the pointer
  // before ~Session joins the coordinator thread that runs this callback.
  ge::Session* session = s.get();
  s->events().on_topology_published = [session, e0, e1](ge::TopologyVersion v) {
    EXPECT_EQ(session->current_topology()->FindEdge("e0"), e0) << "v" << v;
    EXPECT_EQ(session->current_topology()->FindEdge("e1"), e1) << "v" << v;
  };
  for (int round = 0; round < 8; ++round) {
    const std::string n = "slow" + std::to_string(round);
    auto add = s->Apply(ge::Mutation()
                            .AddNode(Node("pass_" + n, "Pass@1.0.0", ge::JsonValue(ge::JsonObject{{"delay_us", ge::JsonValue(1500)}})))
                            .AddNode(Node("sink_" + n, "Sink@1.0.0"))
                            .AddEdge({"src", "out"}, {"pass_" + n, "in"},
                                     {.id = "to_" + n, .queue = {.capacity = 4, .policy = ge::DropPolicy::kDropOldest}})
                            .AddEdge({"pass_" + n, "out"}, {"sink_" + n, "in"}, {.queue = {.capacity = 4}})
                            .Build());
    ASSERT_TRUE(add.ok());
    ExpectSucceeded(f.ops, *add);
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    const std::size_t main_before = f.Sink("sink")->Seqs().size();
    auto rm = s->Apply(RemoveBranch(n, (round % 2) ? ge::RemovePolicy::kFast : ge::RemovePolicy::kDrain));
    ASSERT_TRUE(rm.ok());
    // Watchdog for the drain deadline.
    ge::OperationRecord rec;
    for (int i = 0; i < 2000; ++i) {
      s->Tick();
      if (auto r = f.ops.Get(*rm); r && r->terminal()) {
        rec = *r;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(rec.state, ge::OperationState::kSucceeded) << round << " " << rec.result.ToString();
    EXPECT_TRUE(f.Sink("sink_" + n)->closed) << round;
    EXPECT_GT(f.Sink("sink")->Seqs().size(), main_before) << round;  // main path kept flowing
  }
  StopAndCheck(f, *s);
  ExpectLossless(*f.Sink("sink"));
  EXPECT_EQ(s->current_topology()->nodes().size(), 3U);
  (void)timeouts;
}

// ---------------------------------------------------------------------------
// 10. Burst of disjoint mutations merged into one version (12 §7.1), then
//     a burst of removals; repeated.
// ---------------------------------------------------------------------------

TEST(MutationStressTest, MergedBurstsAddThenRemove) {
  Fixture f;
  ge::ExecutorPool exec(0);
  constexpr std::int64_t kCount = 1500;
  constexpr int kBranches = 6;
  ge::GraphBuilder g("fan");
  auto src = g.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(kCount)}}));
  for (int i = 0; i < kBranches; ++i) {
    const std::string n = std::to_string(i);
    auto p = g.AddNode(Op("Pass@1.0.0"), "p" + n);
    auto k = g.AddNode(Op("Sink@1.0.0"), "sink" + n);
    g.Connect(src.port("out"), p.port("in"));
    g.Connect(p.port("out"), k.port("in"), {.id = "out" + n});
  }
  auto s = f.Create(*g.Build(), exec, {.mutation_merge_limit = kBranches});
  ASSERT_TRUE(s->Start().ok());
  ge::TopologyVersion version = 1;
  auto edge_of = [](int i, int round) {
    return round == 0 ? "out" + std::to_string(i)
                      : "p" + std::to_string(i) + ".out->sink" + std::to_string(i) + ".in";  // bypass auto-name
  };
  for (int round = 0; round < 6; ++round) {
    for (int k = 0; k < 10; ++k) (void)exec.RunOne();
    std::vector<ge::OperationId> adds;
    for (int i = 0; i < kBranches; ++i) {
      const std::string id = "m" + std::to_string(round) + "_" + std::to_string(i);
      auto op = s->Apply(ge::Mutation().InsertChain(edge_of(i, round), {Node(id, "Pass@1.0.0")}).Build());
      ASSERT_TRUE(op.ok());
      adds.push_back(*op);
    }
    ASSERT_TRUE(s->PumpMutations());
    ++version;
    for (int k = 0; k < 200 && !f.ops.Get(adds.back())->terminal(); ++k) {
      (void)exec.RunOne();
      s->Tick();
    }
    for (const ge::OperationId id : adds) {
      const auto rec = WaitOp(f.ops, id);
      EXPECT_EQ(rec.state, ge::OperationState::kSucceeded) << rec.result.ToString();
      EXPECT_EQ(rec.topology_version, version) << round;  // all six merged into one publish
    }
    EXPECT_FALSE(s->PumpMutations());
    for (int k = 0; k < 10; ++k) (void)exec.RunOne();
    std::vector<ge::OperationId> removes;
    for (int i = 0; i < kBranches; ++i) {
      const std::string id = "m" + std::to_string(round) + "_" + std::to_string(i);
      auto op = s->Apply(ge::Mutation().RemoveChain({id}, {.bypass = true}).Build());
      ASSERT_TRUE(op.ok());
      removes.push_back(*op);
    }
    ASSERT_TRUE(s->PumpMutations());
    ++version;
    for (int k = 0; k < 200 && !f.ops.Get(removes.back())->terminal(); ++k) {
      (void)exec.RunOne();
      s->Tick();
    }
    for (const ge::OperationId id : removes) {
      const auto rec = WaitOp(f.ops, id);
      EXPECT_EQ(rec.state, ge::OperationState::kSucceeded) << rec.result.ToString();
      EXPECT_EQ(rec.topology_version, version) << round;
    }
    ASSERT_EQ(s->state(), ge::SessionState::kRunning) << round;  // source must outlive the bursts
  }
  Drain(exec, *s);
  EXPECT_EQ(s->topology_version(), version);
  for (int i = 0; i < kBranches; ++i) {
    EXPECT_EQ(f.Sink("sink" + std::to_string(i))->Seqs(), Iota(static_cast<std::size_t>(kCount))) << i;
    EXPECT_NE(s->current_topology()->FindEdge("p" + std::to_string(i) + ".out->sink" + std::to_string(i) + ".in"), nullptr) << i;
  }
  EXPECT_EQ(s->current_topology()->nodes().size(), 1U + 2U * kBranches);
  EXPECT_EQ(s->scheduler().pending_retirements(), 0U);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

}  // namespace
