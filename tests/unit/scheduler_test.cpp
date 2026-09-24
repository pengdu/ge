#include <ge/cpp/scheduler.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <numeric>
#include <string>

#include "test_operators.h"

namespace {

using namespace ge::test;

// Emits |factor| packets per input (seq = in*factor + k) and treats every
// emit as final: a sync operator relies on 12 §6.2 parking, never retries.
class Burst final : public ge::Operator {
 public:
  explicit Burst(int factor) : factor_(factor) {}
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    for (const ge::PacketRef& in : req.inputs) {
      if (in->is_event()) continue;
      for (int k = 0; k < factor_; ++k) {
        ge::Packet out = *in;
        out.header.seq = (in->header.seq - 1) * static_cast<ge::PacketSeq>(factor_) + static_cast<ge::PacketSeq>(k) + 1;
        const ge::Status s = req.sink->Emit("out", std::move(out));
        if (!s.ok()) return s;  // WOULD_BLOCK would be a contract violation here
      }
    }
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }

 private:
  int factor_;
};

struct Fixture {
  std::shared_ptr<ge::HostBufferPool> pool = ge::HostBufferPool::Create();
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  std::vector<CountingSource*> sources;
  std::vector<PassThrough*> passes;
  std::vector<Collector*> sinks;

  Fixture() {
    factory.Register(Desc("Src@1.0.0", {}, {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)}, true, 1),
                     [this](const ge::OperatorCreateArgs& a) {
                       const auto n = a.options.GetInteger("count").value_or(10);
                       auto s = std::make_shared<CountingSource>(n, pool);
                       sources.push_back(s.get());
                       return Keep(keep, s);
                     });
    // Sync pass-through keeps order only with parallelism 1 (12 §6.1).
    factory.Register(Desc("Pass@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)},
                          false, 1),
                     [this](const ge::OperatorCreateArgs& a) {
                       const auto us = a.options.GetInteger("delay_us").value_or(0);
                       auto p = std::make_shared<PassThrough>(std::chrono::microseconds(us));
                       passes.push_back(p.get());
                       return Keep(keep, p);
                     });
    factory.Register(Desc("ParPass@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput)}, false, 4),
                     [this](const ge::OperatorCreateArgs&) {
                       auto p = std::make_shared<PassThrough>(std::chrono::microseconds(20));
                       passes.push_back(p.get());
                       return Keep(keep, p);
                     });
    factory.Register(Desc("Stateful@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput)}, true, 1),
                     [this](const ge::OperatorCreateArgs&) {
                       auto p = std::make_shared<PassThrough>();
                       passes.push_back(p.get());
                       return Keep(keep, p);
                     });
    factory.Register(Desc("Sink@1.0.0", {BytesPort("in", ge::PortDirection::kInput)}, {}),
                     [this](const ge::OperatorCreateArgs&) {
                       auto c = std::make_shared<Collector>();
                       sinks.push_back(c.get());
                       return Keep(keep, c);
                     });
    factory.Register(Desc("Sink2@1.0.0", {BytesPort("a", ge::PortDirection::kInput), BytesPort("b", ge::PortDirection::kInput, false)}, {}),
                     [this](const ge::OperatorCreateArgs&) {
                       auto c = std::make_shared<Collector>();
                       sinks.push_back(c.get());
                       return Keep(keep, c);
                     });
    factory.Register(Desc("Faulty@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput)}, false, 1),
                     [](const ge::OperatorCreateArgs& a) {
                       return std::make_unique<Faulty>(static_cast<int>(a.options.GetInteger("fail_at").value_or(1)));
                     });
    factory.Register(Desc("BadOpen@1.0.0", {BytesPort("in", ge::PortDirection::kInput)}, {}),
                     [](const ge::OperatorCreateArgs&) { return std::make_unique<FailsToOpen>(); });
    factory.Register(Desc("Burst@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)},
                          false, 1),
                     [](const ge::OperatorCreateArgs& a) {
                       return std::make_unique<Burst>(static_cast<int>(a.options.GetInteger("factor").value_or(4)));
                     });
    RegisterVideoFixture(factory, keep);
  }

  std::shared_ptr<ge::RuntimeTopology> Build(const ge::GraphSpec& spec) {
    auto r = ge::RuntimeTopology::Build(spec, factory, {.session_id = 7, .version = 1});
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return r.ok() ? *r : nullptr;
  }
};

ge::OperatorKey Op(const char* t) { return *ge::OperatorKey::Parse(t); }

// Records the side-band publications the engine forwards through
// SchedulerEvents::on_operator_event, and hands the scheduler a std::function
// that appends to it. The hook fires on an executor thread, so the list is
// mutex-guarded.
class ObservedEvents final {
 public:
  struct Entry {
    std::string node;
    std::string type;
  };
  [[nodiscard]] std::function<void(ge::NodeRuntime&, std::string, ge::Severity, ge::JsonValue)> Hook() {
    return [this](ge::NodeRuntime& node, std::string type, ge::Severity, ge::JsonValue) {
      std::lock_guard lock(mutex_);
      entries_.push_back({node.external_id(), std::move(type)});
    };
  }
  std::vector<Entry> Entries() const {
    std::lock_guard lock(mutex_);
    return entries_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
};


ge::GraphSpec Linear(int nodes, std::int64_t count, ge::DropPolicy policy = ge::DropPolicy::kBlock,
                     std::uint32_t capacity = 64, std::int64_t pass_delay_us = 0) {
  ge::GraphBuilder b("linear");
  auto prev = b.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(count)}}));
  for (int i = 0; i < nodes; ++i) {
    auto p = b.AddNode(Op("Pass@1.0.0"), "p" + std::to_string(i),
                       ge::JsonValue(ge::JsonObject{{"delay_us", ge::JsonValue(pass_delay_us)}}));
    ge::EdgeOptions eo;
    eo.queue.policy = policy;
    eo.queue.capacity = capacity;
    b.Connect(prev.port("out"), p.port("in"), eo);
    prev = p;
  }
  auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  ge::EdgeOptions eo;
  eo.queue.policy = policy;
  eo.queue.capacity = capacity;
  b.Connect(prev.port("out"), sink.port("in"), eo);
  return *b.Build();
}

std::vector<ge::PacketSeq> Iota(std::int64_t n) {
  std::vector<ge::PacketSeq> v(static_cast<std::size_t>(n));
  std::iota(v.begin(), v.end(), 1U);
  return v;
}

TEST(SchedulerTest, LinearPipelineDeliversInOrderAndCloses) {
  Fixture f;
  auto topo = f.Build(Linear(3, 100));
  ASSERT_TRUE(topo);
  ge::ExecutorPool exec(4);
  bool all_closed_event = false;
  ge::Scheduler s(topo, exec, {.on_all_closed = [&] { all_closed_event = true; }});
  ASSERT_TRUE(s.OpenAll().ok());
  for (PassThrough* p : f.passes) EXPECT_TRUE(p->opened);
  s.Start();
  ASSERT_TRUE(s.WaitClosed(5000));
  EXPECT_TRUE(all_closed_event);
  EXPECT_EQ(f.sinks[0]->Seqs(), Iota(100));
  EXPECT_TRUE(f.sinks[0]->flushed);
  EXPECT_TRUE(f.sinks[0]->closed);
  EXPECT_FALSE(f.sinks[0]->fast_close);
  for (PassThrough* p : f.passes) {
    EXPECT_TRUE(p->flushed);
    EXPECT_TRUE(p->closed);
  }
  EXPECT_TRUE(f.sources[0]->closed);
  for (const auto& n : topo->nodes()) EXPECT_EQ(n->state(), ge::NodeState::kClosed) << n->external_id();
  for (const auto& e : topo->edges()) EXPECT_EQ(e->depth(), 0U);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

TEST(SchedulerTest, InlineExecutorIsDeterministic) {
  Fixture f;
  auto topo = f.Build(Linear(2, 20));
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  while (exec.RunPending()) {
  }
  EXPECT_TRUE(s.all_closed());
  EXPECT_EQ(f.sinks[0]->Seqs(), Iota(20));
}

TEST(SchedulerTest, FanoutQueuesAreIndependentAndZeroCopy) {
  Fixture f;
  ge::GraphBuilder b("fan");
  auto src = b.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(50)}}));
  auto fast = b.AddNode(Op("Pass@1.0.0"), "fast");
  auto slow = b.AddNode(Op("Pass@1.0.0"), "slow", ge::JsonValue(ge::JsonObject{{"delay_us", ge::JsonValue(200)}}));
  auto sink_fast = b.AddNode(Op("Sink@1.0.0"), "sink_fast");
  auto sink_slow = b.AddNode(Op("Sink@1.0.0"), "sink_slow");
  ge::EdgeOptions small;
  small.queue.capacity = 4;
  small.queue.policy = ge::DropPolicy::kDropOldest;
  b.Connect(src.port("out"), fast.port("in"), {.id = "to_fast"});
  b.Connect(src.port("out"), slow.port("in"), small);
  b.Connect(fast.port("out"), sink_fast.port("in"));
  b.Connect(slow.port("out"), sink_slow.port("in"));
  auto topo = f.Build(*b.Build());
  ASSERT_TRUE(topo);
  ge::ExecutorPool exec(4);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  ASSERT_TRUE(s.WaitClosed(5000));
  Collector* cf = f.sinks[0];
  Collector* cs = f.sinks[1];
  EXPECT_EQ(cf->Seqs(), Iota(50));  // fast branch complete regardless of slow
  const auto slow_seqs = cs->Seqs();
  EXPECT_LE(slow_seqs.size(), 50U);
  EXPECT_TRUE(std::is_sorted(slow_seqs.begin(), slow_seqs.end()));
  EXPECT_EQ(slow_seqs.back(), 50U);  // last packet always arrives (EOS never dropped before it)
  const auto m = topo->FindEdge("src.out->slow.in")->metrics().Load();
  EXPECT_EQ(m.drop_count + slow_seqs.size(), 50U);
  // Zero-copy: while both branches were live, payloads had >1 ref.
  bool shared = false;
  for (std::uint32_t r : cf->payload_refs) shared = shared || r > 1;
  EXPECT_TRUE(shared);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

// 12 §6.2 for synchronous non-source nodes: a burst that overruns a
// capacity-2 block edge is parked by the engine (Emit returns OK), the node
// is not re-invoked until the parked packets went through, and the sink
// sees every packet in emit order. With threads the retry runs from the
// consumer's Pop path; with the inline executor from the invoke gate.
TEST(SchedulerTest, SyncEmitOverrunIsParkedNotDroppedAndStaysOrdered) {
  for (const std::uint32_t threads : {0u, 3u}) {
    Fixture f;
    ge::GraphBuilder b("burst");
    auto src = b.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(50)}}));
    auto burst = b.AddNode(Op("Burst@1.0.0"), "burst", ge::JsonValue(ge::JsonObject{{"factor", ge::JsonValue(5)}}));
    auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
    ge::EdgeOptions wide, narrow;
    wide.queue.capacity = 8;
    narrow.queue.capacity = 2;
    b.Connect(src.port("out"), burst.port("in"), {.id = "e0", .queue = wide.queue});
    b.Connect(burst.port("out"), sink.port("in"), {.id = "e1", .queue = narrow.queue});
    auto topo = f.Build(*b.Build());
    ASSERT_TRUE(topo);
    ge::ExecutorPool exec(threads);
    ge::Scheduler s(topo, exec);
    ASSERT_TRUE(s.OpenAll().ok());
    s.Start();
    if (threads == 0) {
      for (int i = 0; i < 200000 && !s.all_closed(); ++i) {
        (void)exec.RunPending();
        s.Tick();
      }
    }
    const bool closed = s.WaitClosed(10000);
    if (!closed) {
      // Liveness regression (lost wakeup in Invoke's parked/blocked gate):
      // dump node and edge state so a hang is diagnosable from CI logs.
      std::string dump = "threads=" + std::to_string(threads) + "\n";
      for (const auto& n : topo->nodes()) {
        dump += "  node " + n->external_id() + " state=" + std::string(ge::ToString(n->state())) + "\n";
      }
      for (const auto& e : topo->edges()) {
        const auto m = e->metrics().Load();
        dump += "  edge " + e->external_id() + " state=" + std::string(ge::ToString(e->state())) +
                " depth=" + std::to_string(m.queue_depth) + " pushed=" + std::to_string(m.pushed) +
                " popped=" + std::to_string(m.popped) + "\n";
      }
      ASSERT_TRUE(closed) << dump;
    }
    EXPECT_EQ(f.sinks[0]->Seqs(), Iota(250)) << "threads=" << threads;
    ge::EdgeChannel* e1 = topo->FindEdge("e1");
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(e1->metrics().Load().drop_count, 0U);
    EXPECT_GT(e1->metrics().Load().would_block_count, 0U);
    EXPECT_TRUE(f.sinks[0]->flushed.load());
  }
}

TEST(SchedulerTest, BlockPolicyBackpressuresSourceWithoutLoss) {
  Fixture f;
  // A slow consumer guarantees the source hits a full edge at least once;
  // without the delay a fast machine can drain capacity 2 as quickly as the
  // source fills it and would_block stays 0 (seen on the Linux clang lane).
  auto topo = f.Build(Linear(1, 500, ge::DropPolicy::kBlock, 2, 50));
  ASSERT_TRUE(topo);
  ge::ExecutorPool exec(2);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  ASSERT_TRUE(s.WaitClosed(10000));
  EXPECT_EQ(f.sinks[0]->Seqs(), Iota(500));
  EXPECT_EQ(f.sources[0]->blocked.load(), 0);  // gated before invoke, never mid-emit
  std::uint64_t drops = 0, would_block = 0;
  for (const auto& e : topo->edges()) {
    drops += e->metrics().Load().drop_count;
    would_block += e->metrics().Load().would_block_count;
  }
  EXPECT_EQ(drops, 0U);
  EXPECT_GT(would_block, 0U);
}

TEST(SchedulerTest, ParallelStatelessNodeUsesSlotsWithoutLoss) {
  Fixture f;
  ge::GraphBuilder b("par");
  auto src = b.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(400)}}));
  ge::NodeSpec par_spec;
  par_spec.id = "par";
  par_spec.op = Op("ParPass@1.0.0");
  par_spec.parallelism = 4;
  auto par = b.AddNode(par_spec);
  auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  ge::EdgeOptions in_eo, out_eo;
  in_eo.queue.capacity = 256;
  out_eo.queue.capacity = 1024;
  b.Connect(src.port("out"), par.port("in"), in_eo);
  b.Connect(par.port("out"), sink.port("in"), out_eo);
  auto topo = f.Build(*b.Build());
  ASSERT_TRUE(topo);
  EXPECT_EQ(topo->FindNode("par")->max_parallelism(), 4U);
  ge::ExecutorPool exec(6);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  ASSERT_TRUE(s.WaitClosed(10000));
  auto seqs = f.sinks[0]->Seqs();
  std::sort(seqs.begin(), seqs.end());
  // Parallel node: no loss, order not guaranteed. On mismatch name the
  // missing / duplicated seqs (gtest truncates long vectors).
  const auto want = Iota(400);
  if (seqs != want) {
    std::string missing, extra;
    for (const ge::PacketSeq v : want) {
      if (!std::binary_search(seqs.begin(), seqs.end(), v)) missing += std::to_string(v) + " ";
    }
    for (std::size_t i = 1; i < seqs.size(); ++i) {
      if (seqs[i] == seqs[i - 1]) extra += std::to_string(seqs[i]) + " ";
    }
    ge::NodeRuntime* par_node = topo->FindNode("par");
    ADD_FAILURE() << "got " << seqs.size() << " of 400; missing: " << missing << "; extra: " << extra
                  << "; par packets_in=" << par_node->metrics().packets_in.load()
                  << " invocations=" << par_node->metrics().invocations.load() << " flushed="
                  << f.sinks[0]->flushed.load();
  }
}

TEST(SchedulerTest, TwoInputSinkAnyPolicyEndsWhenRequiredEos) {
  Fixture f;
  ge::GraphBuilder b("two");
  auto s1 = b.AddNode(Op("Src@1.0.0"), "s1", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(5)}}));
  auto s2 = b.AddNode(Op("Src@1.0.0"), "s2", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(3)}}));
  auto sink = b.AddNode(Op("Sink2@1.0.0"), "sink");
  b.Connect(s1.port("out"), sink.port("a"));
  b.Connect(s2.port("out"), sink.port("b"));
  auto topo = f.Build(*b.Build());
  ASSERT_TRUE(topo);
  ge::ExecutorPool exec(2);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  ASSERT_TRUE(s.WaitClosed(5000));
  const auto ports = f.sinks[0]->Ports();
  EXPECT_EQ(std::count(ports.begin(), ports.end(), "a"), 5);
  EXPECT_LE(std::count(ports.begin(), ports.end(), "b"), 3);
}

TEST(SchedulerTest, FastStopClosesWithoutFlush) {
  Fixture f;
  auto topo = f.Build(Linear(2, 1'000'000));
  ASSERT_TRUE(topo);
  ge::ExecutorPool exec(3);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  s.Stop(true);
  ASSERT_TRUE(s.WaitClosed(5000));
  EXPECT_TRUE(f.sinks[0]->closed);
  EXPECT_TRUE(f.sinks[0]->fast_close);
  EXPECT_FALSE(f.sinks[0]->flushed);
  for (const auto& e : topo->edges()) EXPECT_EQ(e->state(), ge::EdgeState::kRetired);
  EXPECT_GT(f.sinks[0]->Seqs().size(), 0U);
  for (const auto& e : topo->edges()) EXPECT_EQ(e->depth(), 0U);
  exec.Stop();
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

TEST(SchedulerTest, GracefulStopInjectsEosAtSources) {
  Fixture f;
  auto topo = f.Build(Linear(1, 1'000'000));
  ge::ExecutorPool exec(2);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  s.Stop(false);
  ASSERT_TRUE(s.WaitClosed(5000));
  EXPECT_TRUE(f.sinks[0]->flushed);
  EXPECT_FALSE(f.sinks[0]->fast_close);
  const auto seqs = f.sinks[0]->Seqs();
  EXPECT_EQ(seqs, Iota(static_cast<std::int64_t>(seqs.size())));  // no loss, no reorder
}

TEST(SchedulerTest, PauseResume) {
  Fixture f;
  auto topo = f.Build(Linear(1, 100000));
  ge::ExecutorPool exec(2);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  s.Pause();
  // Pause gates the source only: whatever is already queued on the edges
  // still drains, so wait until nothing is in flight before asserting
  // that nothing more arrives.
  ASSERT_TRUE(WaitDrained(*topo, std::chrono::seconds(5)));
  const std::size_t a = f.sinks[0]->Seqs().size();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const std::size_t b = f.sinks[0]->Seqs().size();
  EXPECT_EQ(a, b);
  s.Resume();
  bool grew = false;
  for (int i = 0; i < 200 && !grew; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    grew = f.sinks[0]->Seqs().size() > b;
  }
  EXPECT_TRUE(grew);
  s.Stop(true);
  ASSERT_TRUE(s.WaitClosed(5000));
}

TEST(SchedulerTest, NodeFailureIsReported) {
  Fixture f;
  ge::GraphBuilder b("f");
  auto src = b.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(10)}}));
  auto bad = b.AddNode(Op("Faulty@1.0.0"), "bad", ge::JsonValue(ge::JsonObject{{"fail_at", ge::JsonValue(3)}}));
  auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  b.Connect(src.port("out"), bad.port("in"));
  b.Connect(bad.port("out"), sink.port("in"));
  auto topo = f.Build(*b.Build());
  ge::ExecutorPool exec(2);
  std::mutex m;
  std::string failed_node;
  ge::Scheduler s(topo, exec, {.on_node_failed = [&](ge::NodeRuntime& n, const ge::Status&) {
                    std::lock_guard lock(m);
                    failed_node = n.external_id();
                  }});
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  {
    std::lock_guard lock(m);
    EXPECT_EQ(failed_node, "bad");
  }
  EXPECT_EQ(topo->FindNode("bad")->state(), ge::NodeState::kFailed);
  EXPECT_EQ(topo->FindNode("bad")->failure().code(), GE_STATUS_INTERNAL);
  EXPECT_EQ(f.sinks[0]->Seqs().size(), 2U);
  s.Stop(true);
  ASSERT_TRUE(s.WaitClosed(5000));
}

TEST(SchedulerTest, OpenFailureRollsBack) {
  Fixture f;
  ge::GraphBuilder b("o");
  auto src = b.AddNode(Op("Src@1.0.0"), "src");
  auto p = b.AddNode(Op("Pass@1.0.0"), "p");
  auto bad = b.AddNode(Op("BadOpen@1.0.0"), "bad");
  b.Connect(src.port("out"), p.port("in"));
  b.Connect(p.port("out"), bad.port("in"));
  auto topo = f.Build(*b.Build());
  ge::ExecutorPool exec(1);
  ge::Scheduler s(topo, exec);
  const ge::Status st = s.OpenAll();
  EXPECT_EQ(st.code(), GE_STATUS_NODE_WARMUP_FAILED);
  EXPECT_TRUE(f.passes[0]->closed);  // previously opened nodes closed
  EXPECT_EQ(topo->FindNode("bad")->state(), ge::NodeState::kFailed);
}

TEST(SchedulerTest, ParameterUpdateAppliesAtNextPacket) {
  Fixture f;
  auto topo = f.Build(Linear(1, 200));
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  ge::NodeRuntime* p0 = topo->FindNode("p0");
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(exec.RunOne());
  EXPECT_EQ(p0->parameters().Current()->version, 1U);
  ge::ParameterUpdateRequest upd;
  upd.values = ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(7)}});
  ASSERT_TRUE(p0->parameters().Submit(upd, p0->capability().parameters.hot_updatable).ok());
  EXPECT_TRUE(p0->parameters().has_pending());
  ge::ParameterUpdateRequest bad;
  bad.values = ge::JsonValue(ge::JsonObject{{"model", ge::JsonValue("x")}});
  EXPECT_EQ(p0->parameters().Submit(bad, p0->capability().parameters.hot_updatable).status().code(),
            GE_STATUS_PARAMETER_UNSUPPORTED);
  while (exec.RunPending()) {
  }
  EXPECT_TRUE(s.all_closed());
  EXPECT_EQ(p0->parameters().Current()->version, 2U);
  EXPECT_EQ(f.passes[0]->gain_seen.load(), 7);
  EXPECT_EQ(f.passes[0]->last_param_version.load(), 2U);
  const auto hist = p0->parameters().history();
  ASSERT_EQ(hist.size(), 1U);
  EXPECT_EQ(hist[0].version, 2U);
  EXPECT_GT(hist[0].effective_after_seq, 0U);
}

TEST(SchedulerTest, StatefulNodeNeverRunsConcurrently) {
  Fixture f;
  ge::GraphBuilder b("st");
  auto src = b.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(2000)}}));
  auto st = b.AddNode(Op("Stateful@1.0.0"), "st");
  auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  b.Connect(src.port("out"), st.port("in"));
  b.Connect(st.port("out"), sink.port("in"));
  auto topo = f.Build(*b.Build());
  EXPECT_EQ(topo->FindNode("st")->max_parallelism(), 1U);
  ge::ExecutorPool exec(6);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  ASSERT_TRUE(s.WaitClosed(10000));
  EXPECT_EQ(f.sinks[0]->Seqs(), Iota(2000));
}

TEST(SchedulerTest, TenNodeLatencyAndSchedulingOverhead) {
  Fixture f;
  constexpr std::int64_t kPackets = 20000;
  auto topo = f.Build(Linear(10, kPackets, ge::DropPolicy::kBlock, 256));
  ge::ExecutorPool exec(0);  // inline: measures framework cost only
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  const auto start = std::chrono::steady_clock::now();
  s.Start();
  while (exec.RunPending()) {
  }
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - start).count();
  ASSERT_TRUE(s.all_closed());
  ASSERT_EQ(f.sinks[0]->Seqs().size(), static_cast<std::size_t>(kPackets));
  std::uint64_t invocations = 0;
  for (const auto& n : topo->nodes()) invocations += n->metrics().invocations.load();
  const double per_packet_us = static_cast<double>(ns) / 1000.0 / static_cast<double>(kPackets);
  const double per_invoke_us = static_cast<double>(ns) / 1000.0 / static_cast<double>(invocations);
  RecordProperty("per_packet_us", std::to_string(per_packet_us));
  RecordProperty("per_invoke_us", std::to_string(per_invoke_us));
#ifdef NDEBUG
  // The budgets (PERF-1) are for an unloaded machine; under the contention
  // stress runner (8 copies per core) they are recorded but not enforced.
  if (std::getenv("GE_STRESS") == nullptr) {
    EXPECT_LT(per_packet_us, 50.0) << "10-node pipeline added latency per packet";
    EXPECT_LT(per_invoke_us, 2.0) << "scheduling overhead per node invocation";
  }
#endif
}

TEST(SchedulerTest, EventPacketsReachCollectorWithTheirSourcePort) {
  Fixture f;
  ge::GraphBuilder b("event-ports");
  auto src = b.AddNode(Op("Src@1.0.0"), "src",
                       ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(std::int64_t{3})}}));
  auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  b.Connect(src.port("out"), sink.port("in"), {.id = "e0"});
  auto topo = f.Build(*b.Build());
  ASSERT_TRUE(topo);
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  while (exec.RunPending()) {
  }
  const std::vector<std::string> ports = f.sinks[0]->Ports();
  const std::vector<ge::PacketSeq> seqs = f.sinks[0]->Seqs();
  ASSERT_EQ(ports.size(), seqs.size());
  for (const std::string& p : ports) EXPECT_EQ(p, "in");
}

TEST(SchedulerTest, OnlyVideoContractsCountAsVideoRoutes) {
  Fixture f;
  ge::GraphBuilder b("video-route");
  auto src = b.AddNode(Op("VSrc@1.0.0"), "src",
                       ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(std::int64_t{1})}}));
  auto vout = b.AddNode(Op("VSink@1.0.0"), "vout");
  auto bout = b.AddNode(Op("Sink@1.0.0"), "bout");
  b.Connect(src.port("out"), vout.port("in"), {.id = "e0"});
  b.Connect(src.port("bytes"), bout.port("in"), {.id = "e1"});
  auto topo = f.Build(*b.Build());
  ASSERT_TRUE(topo);
  const std::vector<ge::RouteEntry>* video = topo->RoutesFor(topo->FindNode("src")->id(), "out");
  ASSERT_NE(video, nullptr);
  ASSERT_FALSE(video->empty());
  EXPECT_TRUE(ge::IsVideoRoute(video->front().contract));
  const std::vector<ge::RouteEntry>* bytes = topo->RoutesFor(topo->FindNode("src")->id(), "bytes");
  ASSERT_NE(bytes, nullptr);
  ASSERT_FALSE(bytes->empty());
  EXPECT_FALSE(ge::IsVideoRoute(bytes->front().contract));
}

// A sink that records what each port received and can refuse "out" until told
// otherwise, so one Process call sees one port blocked and the other free.
class RecordingBlockSink final : public ge::EmitSink {
 public:
  ge::Status Emit(std::string_view port, ge::Packet packet) override {
    if (port == "out" && block_out) return ge::Status::WouldBlock();
    seqs[std::string(port)].push_back(packet.header.seq);
    return ge::Status::Ok();
  }
  bool block_out = false;
  std::map<std::string, std::vector<ge::PacketSeq>> seqs;
};

// Regression: the shared video-source helper used to re-emit a seq on the
// port that had already accepted it, whenever the *other* port blocked. A
// re-emitted seq is exactly what a duplicate-publication check would misread
// as a second keyframe, so each port's emitted seqs must be strictly
// increasing with no duplicates.
TEST(SchedulerTest, VideoSourceNeverReemitsASeqUnderPartialBackpressure) {
  VideoCountSource src(3);
  RecordingBlockSink sink;
  ge::ProcessRequest req;
  req.sink = &sink;

  // Both ports accept: seq 1 goes out on each.
  ASSERT_TRUE(src.Process(req).ok());
  EXPECT_EQ(sink.seqs["out"], (std::vector<ge::PacketSeq>{1}));
  EXPECT_EQ(sink.seqs["bytes"], (std::vector<ge::PacketSeq>{1}));

  // "out" blocks: the seq that port already delivered must not repeat, while
  // "bytes" is free to advance to seq 2.
  sink.block_out = true;
  ASSERT_TRUE(src.Process(req).ok());
  EXPECT_EQ(sink.seqs["out"], (std::vector<ge::PacketSeq>{1}));
  EXPECT_EQ(sink.seqs["bytes"], (std::vector<ge::PacketSeq>{1, 2}));

  // Unblocked: "out" delivers the seq it was holding (2), not a repeat of 1.
  sink.block_out = false;
  ASSERT_TRUE(src.Process(req).ok());
  EXPECT_EQ(sink.seqs["out"], (std::vector<ge::PacketSeq>{1, 2}));

  // Drain, then the source is exhausted with both ports having seen 1..3.
  bool exhausted = false;
  for (int i = 0; i < 10 && !exhausted; ++i) {
    const auto r = src.Process(req);
    ASSERT_TRUE(r.ok()) << r.status().ToString();
    exhausted = (*r == ge::ProcessResult::kExhausted);
  }
  EXPECT_TRUE(exhausted);
  EXPECT_EQ(sink.seqs["out"], (std::vector<ge::PacketSeq>{1, 2, 3}));
  EXPECT_EQ(sink.seqs["bytes"], (std::vector<ge::PacketSeq>{1, 2, 3}));

  for (const auto& [port, s] : sink.seqs) {
    EXPECT_TRUE(std::is_sorted(s.begin(), s.end())) << port;
    EXPECT_EQ(std::adjacent_find(s.begin(), s.end()), s.end()) << port;
  }
}

// EVT-4: a "media_format_changed" publication whose detail names the keyframe
// it belongs to reaches the consumer as an event Packet placed immediately
// before that keyframe, on the same edge and in one Process call.
TEST(SchedulerTest, FormatEventArrivesBeforeItsKeyframe) {
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* sink = nullptr;
  RegisterFormatAnnouncerFixture(factory, keep, /*key_seq=*/7, /*detail_seq=*/7, &sink);
  ge::GraphBuilder b("format-frontier");
  auto src = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto col = b.AddNode(Op("Collect@1.0.0"), "collect");
  b.Connect(src.port("out"), col.port("in"), {.id = "e0"});
  auto r = ge::RuntimeTopology::Build(*b.Build(), factory, {.session_id = 11, .version = 1});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto topo = *r;
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  while (exec.RunPending()) {
  }
  ASSERT_NE(sink, nullptr);
  const std::vector<EventAwareCollector::Entry> got = sink->Entries();
  ASSERT_EQ(got.size(), 2U) << "expected the event Packet and then the keyframe";
  EXPECT_TRUE(got[0].event);
  EXPECT_EQ(got[0].seq, 7U);
  EXPECT_EQ(got[0].port, "in");
  EXPECT_FALSE(got[1].event);
  EXPECT_EQ(got[1].seq, 7U);
  EXPECT_EQ(got[1].port, "in");
  // The binding was consumed, so nothing was left dangling at call end.
  EXPECT_EQ(topo->FindNode("announce")->metrics().format_events_unmirrored.load(), 0U);
}

// EVT-4: when the publication names a keyframe the node never emits, the
// event stays side-band only. The binding is a promise about a specific
// keyframe, not a claim that "some packet" carried it.
TEST(SchedulerTest, UnboundFormatEventStaysSideBandOnly) {
  ObservedEvents observed;
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* sink = nullptr;
  // Detail binds seq 7; the node emits seq 8.
  RegisterFormatAnnouncerFixture(factory, keep, /*key_seq=*/8, /*detail_seq=*/7, &sink);
  ge::GraphBuilder b("format-unbound");
  auto src = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto col = b.AddNode(Op("Collect@1.0.0"), "collect");
  b.Connect(src.port("out"), col.port("in"), {.id = "e0"});
  auto r = ge::RuntimeTopology::Build(*b.Build(), factory, {.session_id = 11, .version = 1});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto topo = *r;
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec, ge::SchedulerEvents{.on_operator_event = observed.Hook()});
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  while (exec.RunPending()) {
  }
  ASSERT_NE(sink, nullptr);
  const std::vector<EventAwareCollector::Entry> got = sink->Entries();
  ASSERT_EQ(got.size(), 1U) << "the keyframe only: nothing binds to it";
  EXPECT_FALSE(got[0].event);
  EXPECT_EQ(got[0].seq, 8U);
  // The side-band copy is unaffected by the missing binding.
  const std::vector<ObservedEvents::Entry> side = observed.Entries();
  ASSERT_EQ(side.size(), 1U);
  EXPECT_EQ(side[0].type, "media_format_changed");
  // The candidate never found its keyframe, so the call-end counter advanced.
  EXPECT_EQ(topo->FindNode("announce")->metrics().format_events_unmirrored.load(), 1U);
}

// EVT-4: the binding is keyframe-specific, not seq-specific. A data packet
// carrying the bound seq must not consume the candidate, or a downstream
// would see a format change attached to a frame that is not a keyframe.
TEST(SchedulerTest, NonKeyframeSeqDoesNotConsumeCandidate) {
  ObservedEvents observed;
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* sink = nullptr;
  // Same seq as the detail, but emitted without GE_PACKET_FLAG_KEYFRAME.
  RegisterFormatAnnouncerFixture(factory, keep, /*key_seq=*/7, /*detail_seq=*/7, &sink,
                                 /*keyframe=*/false);
  ge::GraphBuilder b("format-non-keyframe");
  auto src = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto col = b.AddNode(Op("Collect@1.0.0"), "collect");
  b.Connect(src.port("out"), col.port("in"), {.id = "e0"});
  auto r = ge::RuntimeTopology::Build(*b.Build(), factory, {.session_id = 11, .version = 1});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto topo = *r;
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec, ge::SchedulerEvents{.on_operator_event = observed.Hook()});
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  while (exec.RunPending()) {
  }
  ASSERT_NE(sink, nullptr);
  const std::vector<EventAwareCollector::Entry> got = sink->Entries();
  ASSERT_EQ(got.size(), 1U) << "the non-keyframe packet only";
  EXPECT_FALSE(got[0].event);
  EXPECT_EQ(got[0].seq, 7U);
  ASSERT_EQ(observed.Entries().size(), 1U);
  EXPECT_EQ(observed.Entries()[0].type, "media_format_changed");
  // A seq-7 packet existed, and the candidate still went unmatched.
  EXPECT_EQ(topo->FindNode("announce")->metrics().format_events_unmirrored.load(), 1U);
}

// EVT-4: mirroring must not be the price of the side-band contract. With no
// subscriber the observation hook is absent, and the in-band half still has
// to work -- the binding is only about who receives a mirror, not about
// whether the publication happened.
TEST(SchedulerTest, FormatEventMirrorsWithoutASideBandSubscriber) {
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* sink = nullptr;
  RegisterFormatAnnouncerFixture(factory, keep, /*key_seq=*/7, /*detail_seq=*/7, &sink);
  ge::GraphBuilder b("format-no-subscriber");
  auto src = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto col = b.AddNode(Op("Collect@1.0.0"), "collect");
  b.Connect(src.port("out"), col.port("in"), {.id = "e0"});
  auto r = ge::RuntimeTopology::Build(*b.Build(), factory, {.session_id = 11, .version = 1});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto topo = *r;
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec);  // no on_operator_event
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  while (exec.RunPending()) {
  }
  ASSERT_NE(sink, nullptr);
  const std::vector<EventAwareCollector::Entry> got = sink->Entries();
  ASSERT_EQ(got.size(), 2U) << "the in-band mirror does not depend on the hook";
  EXPECT_TRUE(got[0].event);
  EXPECT_EQ(got[0].seq, 7U);
  EXPECT_FALSE(got[1].event);
  EXPECT_EQ(topo->FindNode("announce")->metrics().format_events_unmirrored.load(), 0U);
}

}  // namespace
