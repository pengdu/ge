#include <ge/cpp/scheduler.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <numeric>
#include <string>

#include <ge/cpp/plugin_operator.h>

#include "test_operators.h"

namespace {

using namespace ge::test;

// Bounded condition poll (the repo's WaitDrained shape): no timed sleep is
// ever the assertion, the condition is; the deadline only turns a liveness
// bug into a test failure instead of a hang.
bool WaitUntil(const std::function<bool()>& pred,
               std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

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

// Rebuilt (was vacuous): the old body's source never emitted a
// GE_PACKET_FLAG_EVENT packet, so its event-port loop never ran and it passed
// with the event plumbing reverted. Now a real event Packet (the format
// mirror) must arrive attributed to the exact input port whose edge carried
// the matched keyframe -- and that port is deliberately the *second*-declared
// one ("right"), so a blank name and a "first declared port" fallback both
// fail here, which the sibling-port test (whose event lands on the
// first-declared "in") cannot distinguish.
TEST(SchedulerTest, EventPacketsReachCollectorWithTheirSourcePort) {
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* unused = nullptr;
  // Announcer: candidate bound to seq 7; "out2" emits keyframe 99, "out"
  // emits the bound keyframe 7.
  RegisterFormatAnnouncerFixture(factory, keep, /*key_seq=*/7, /*detail_seq=*/7, &unused,
                                 /*keyframe=*/true, /*two_video_outputs=*/true,
                                 /*out2_seq=*/99);
  EventAwareCollector* sink = nullptr;
  factory.Register(Desc("CollectLR@1.0.0",
                        {VideoPort("left", ge::PortDirection::kInput, {"NV12"}),
                         VideoPort("right", ge::PortDirection::kInput, {"NV12"})},
                        {}),
                   [&keep, &sink](const ge::OperatorCreateArgs&) {
                     auto c = std::make_shared<EventAwareCollector>();
                     sink = c.get();
                     return Keep(keep, std::move(c));
                   });
  ge::GraphBuilder b("event-ports");
  auto src = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto col = b.AddNode(Op("CollectLR@1.0.0"), "collect");
  b.Connect(src.port("out2"), col.port("left"), {.id = "e_left"});
  b.Connect(src.port("out"), col.port("right"), {.id = "e_right"});
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
  // The sibling keyframe 99 (on "left"), the event, and the keyframe 7
  // (both on "right").
  ASSERT_EQ(got.size(), 3U);
  const auto ev = std::find_if(got.begin(), got.end(),
                               [](const EventAwareCollector::Entry& e) { return e.event; });
  ASSERT_NE(ev, got.end()) << "an event Packet must exist for this test to test anything";
  EXPECT_EQ(std::count_if(got.begin(), got.end(),
                          [](const EventAwareCollector::Entry& e) { return e.event; }),
            1);
  EXPECT_EQ(ev->seq, 7U);
  EXPECT_EQ(ev->port, "right") << "event-port attribution lost between InputBatch and ProcessRequest";
  // The event still precedes its own keyframe on that port.
  const auto kf = std::find_if(std::next(ev), got.end(), [](const EventAwareCollector::Entry& e) {
    return !e.event && e.seq == 7U;
  });
  ASSERT_NE(kf, got.end());
  EXPECT_EQ(kf->port, "right");
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

// EVT-4 (Ruling 1): an event mirrored on one output port must not appear on a
// sibling video output of the same node. "out2" emits a keyframe at seq 99 while
// the candidate is bound to seq 7 on "out", so the mirror has to stay on "out".
// Note this test cannot discriminate the `video_port` gate: with two video
// outputs, either port satisfies a *port-kind* predicate, so seq-only keying
// passes here too. SchedulerTest.OpaqueOutputDoesNotConsumeTheCandidate is the
// guard for that gate; this one pins the fan-out contract.
TEST(SchedulerTest, SiblingVideoOutputDoesNotGetTheEvent) {
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* sink = nullptr;
  // The single candidate names seq 7; "out2" emits seq 99, "out" emits 7.
  RegisterFormatAnnouncerFixture(factory, keep, /*key_seq=*/7, /*detail_seq=*/7, &sink,
                                 /*keyframe=*/true, /*two_video_outputs=*/true,
                                 /*out2_seq=*/99);
  ge::GraphBuilder b("format-sibling-port");
  auto src = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto col = b.AddNode(Op("Collect@1.0.0"), "collect");
  b.Connect(src.port("out"), col.port("in"), {.id = "e0"});
  b.Connect(src.port("out2"), col.port("in2"), {.id = "e1"});
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
  ASSERT_EQ(got.size(), 3U) << "sibling keyframe 7, then event(7) and keyframe(7) on 'out'";
  // Exactly one event Packet, and it arrived on the port that emitted the
  // bound keyframe -- not on the sibling that emitted the same seq earlier.
  ASSERT_EQ(std::count_if(got.begin(), got.end(),
                          [](const EventAwareCollector::Entry& e) { return e.event; }),
            1);
  const auto ev = std::find_if(got.begin(), got.end(),
                               [](const EventAwareCollector::Entry& e) { return e.event; });
  ASSERT_NE(ev, got.end());
  EXPECT_EQ(ev->seq, 7U);
  EXPECT_EQ(ev->port, "in");
  // Nothing precedes that event: in particular the sibling's keyframe did not
  // arrive carrying a mirrored event.
  EXPECT_FALSE(std::any_of(got.begin(), ev,
                           [](const EventAwareCollector::Entry& e) { return e.event; }))
      << "the sibling keyframe must not be preceded by an event Packet";
  EXPECT_EQ(topo->FindNode("announce")->metrics().format_events_unmirrored.load(), 0U);
}

// EVT-4 (Ruling 1): a candidate is consumed only by a keyframe on a port that
// carries a *video* route. A node with one video and one opaque output that
// emits the bound seq on the opaque port first must leave the candidate
// staged: this is the case the `video_port` predicate exists for, and it is
// the only graph shape that can distinguish the gate from seq-only keying.
TEST(SchedulerTest, OpaqueOutputDoesNotConsumeTheCandidate) {
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* sink = nullptr;
  // Candidate names seq 7. "bytes" (opaque) emits seq 7 first; "out" (video)
  // emits the bound seq 7 afterwards.
  RegisterFormatAnnouncerFixture(factory, keep, /*key_seq=*/7, /*detail_seq=*/7, &sink,
                                 /*keyframe=*/true, /*two_video_outputs=*/false,
                                 /*out2_seq=*/0, /*hooks=*/{},
                                 /*opaque_output=*/true, /*bytes_seq=*/7);
  ge::GraphBuilder b("format-opaque-port");
  auto src = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto col = b.AddNode(Op("Collect@1.0.0"), "collect");
  b.Connect(src.port("out"), col.port("in"), {.id = "e0"});
  b.Connect(src.port("bytes"), col.port("bytes"), {.id = "e1"});
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
  ASSERT_EQ(got.size(), 3U) << "opaque keyframe 7, then event(7) and keyframe(7) on 'out'";
  // The opaque port's keyframe did not swallow the candidate: the mirror
  // still reached the video port, which is what emitted the bound keyframe.
  ASSERT_EQ(std::count_if(got.begin(), got.end(),
                          [](const EventAwareCollector::Entry& e) { return e.event; }),
            1);
  const auto ev = std::find_if(got.begin(), got.end(),
                               [](const EventAwareCollector::Entry& e) { return e.event; });
  ASSERT_NE(ev, got.end());
  EXPECT_EQ(ev->seq, 7U);
  EXPECT_EQ(ev->port, "in");
  EXPECT_EQ(topo->FindNode("announce")->metrics().format_events_unmirrored.load(), 0U);
}

// EVT-4 (Critical 1): a staged candidate is consumed only once its event
// Packet is really pushed. If that push fails the keyframe behind it is not
// delivered and the candidate stays staged, so the call-end counter still
// reports it -- an unmatched binding must never be both lost and uncounted.
TEST(SchedulerTest, FailedEventPushKeepsTheCandidateCounted) {
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* sink = nullptr;
  ge::RuntimeTopology* topo_ptr = nullptr;
  // The second hook runs after the publication is staged and before the
  // keyframe emit; fast-retiring the node makes PacketRouter::Prepare reject
  // the event Packet with kCancelled. That is the reachable way a push on a
  // live invocation fails.
  std::vector<std::function<void()>> hooks{[] {},
                                           [&topo_ptr] {
                                             ge::NodeRuntime& n = *topo_ptr->FindNode("announce");
                                             n.MarkCancelled();
                                           }};
  RegisterFormatAnnouncerFixture(factory, keep, /*key_seq=*/7, /*detail_seq=*/7, &sink,
                                 /*keyframe=*/true, /*two_video_outputs=*/false,
                                 /*out2_seq=*/0, std::move(hooks));
  ge::GraphBuilder b("format-push-failure");
  auto src = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto col = b.AddNode(Op("Collect@1.0.0"), "collect");
  b.Connect(src.port("out"), col.port("in"), {.id = "e0"});
  auto r = ge::RuntimeTopology::Build(*b.Build(), factory, {.session_id = 11, .version = 1});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto topo = *r;
  topo_ptr = topo.get();
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  while (exec.RunPending()) {
  }
  ASSERT_NE(sink, nullptr);
  // The event push failed, so the keyframe behind it was not emitted either.
  EXPECT_EQ(sink->Entries().size(), 0U) << "a failed event push must skip the keyframe";
  // ...and the candidate it was binding is still counted as unmirrored.
  EXPECT_EQ(topo->FindNode("announce")->metrics().format_events_unmirrored.load(), 1U);
}

// ---------------------------------------------------------------------------
// Task 4: fan-out, mixed outputs, and failure-mode coverage (EVT-4).
// ---------------------------------------------------------------------------

// Review Focus #5: on a full kBlock edge the mirrored event is parked and the
// keyframe queues behind it (same-edge FIFO), so no consumer ever sees the
// keyframe first and nothing is lost.
//
//   announce --e0(capacity 1, kBlock)--> slow  (gated collector)
//   announce --e1(capacity 64)---------> fast
//
// The brief's two-entry skeleton cannot make the edge *provably* full: the
// event and the keyframe are pushed back-to-back inside one Sink::Emit call,
// so with an empty edge the event itself would fill it and nothing pins the
// parked path. Two plain data packets (5, 6) are emitted first instead: the
// gated slow collector pops 5 and blocks inside Process(5), the hook waits
// until it is inside that call, and 6 -- which cannot be delivered while the
// consumer is still in Process(5) -- plus the event and the keyframe all line
// up behind it. Both collectors then must record [5, 6, event(7), keyframe(7)].
TEST(FormatEventTest, EveryVideoEdgeSeesEventThenKeyframeEvenWhenOneIsFull) {
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* fast = nullptr;
  EventAwareCollector* slow = nullptr;
  // The first pre-roll packet fills the capacity-1 edge, the consumer pops it
  // and blocks inside Process on that same call, so from then on every push
  // on this edge -- the second pre-roll packet, the mirrored event, the
  // keyframe -- is parked in FIFO order. Gating on `entered` is the
  // deterministic form: a parked packet is *held* by the scheduler and the
  // channel is empty meanwhile, so "the edge's depth is 1" is not a state
  // this test can observe (probed: it oscillates 0/1, making that condition
  // unsatisfiable).
  std::vector<std::function<void()>> hooks{[&slow] {
    const bool ok = WaitUntil([&] { return slow != nullptr && slow->entered.load() >= 1; });
    ASSERT_TRUE(ok) << "the slow consumer never entered Process: the test lost its gate";
  }};
  RegisterFormatAnnouncerFixture(factory, keep, /*key_seq=*/7, /*detail_seq=*/7, &fast,
                                 /*keyframe=*/true, /*two_video_outputs=*/false,
                                 /*out2_seq=*/0, std::move(hooks), /*opaque_output=*/false,
                                 /*bytes_seq=*/0, /*repeat=*/1, /*pre_seqs=*/{5, 6});
  factory.Register(Desc("SlowCollect@1.0.0",
                        {VideoPort("in", ge::PortDirection::kInput, {"NV12"})}, {}),
                   [&keep, &slow](const ge::OperatorCreateArgs&) {
                     auto c = std::make_shared<EventAwareCollector>(/*gated=*/true);
                     slow = c.get();
                     return Keep(keep, std::move(c));
                   });
  ge::GraphBuilder b("format-fanout-full-edge");
  auto src = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto sl = b.AddNode(Op("SlowCollect@1.0.0"), "slow");
  auto fa = b.AddNode(Op("Collect@1.0.0"), "fast");
  ge::EdgeOptions narrow;
  narrow.queue.capacity = 1;
  narrow.queue.policy = ge::DropPolicy::kBlock;
  b.Connect(src.port("out"), sl.port("in"), {.id = "e0", .queue = narrow.queue});
  b.Connect(src.port("out"), fa.port("in"), {.id = "e1"});
  auto r = ge::RuntimeTopology::Build(*b.Build(), factory, {.session_id = 11, .version = 1});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto topo = *r;
  ge::ExecutorPool exec(3);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  // The fast branch proves both pushes happened (the slow branch is still
  // gated at this point, so its edge really was full for them) ...
  const bool fast_saw_all = WaitUntil([&] { return fast != nullptr && fast->Entries().size() >= 4; });
  const std::uint64_t would_block = topo->FindEdge("e0")->metrics().Load().would_block_count;
  // ... and only then is the slow branch released. Release unconditionally,
  // *before* any assertion: the gated collector is blocked inside Process, so
  // an ASSERT that returns early would leave the executor's thread stuck and
  // turn a plain regression into a teardown deadlock instead of a test
  // failure. The diagnosis is asserted below, after the gate is open.
  ASSERT_NE(slow, nullptr);
  slow->Release();
  ASSERT_TRUE(fast_saw_all) << "fast branch did not see the event and its keyframe";
  EXPECT_GE(would_block, 1U) << "the slow edge was never full: the test lost its point";
  ASSERT_TRUE(s.WaitClosed(10000));
  for (const auto* c : {fast, slow}) {
    const std::vector<EventAwareCollector::Entry> got = c->Entries();
    ASSERT_EQ(got.size(), 4U) << (c == fast ? "fast" : "slow");
    EXPECT_FALSE(got[0].event) << (c == fast ? "fast" : "slow");
    EXPECT_EQ(got[0].seq, 5U);
    EXPECT_FALSE(got[1].event) << (c == fast ? "fast" : "slow");
    EXPECT_EQ(got[1].seq, 6U);
    EXPECT_TRUE(got[2].event) << (c == fast ? "fast" : "slow");
    EXPECT_EQ(got[2].seq, 7U);
    EXPECT_FALSE(got[3].event) << (c == fast ? "fast" : "slow");
    EXPECT_EQ(got[3].seq, 7U);
  }
  EXPECT_EQ(topo->FindEdge("e0")->metrics().Load().drop_count, 0U);
  EXPECT_EQ(topo->FindNode("announce")->metrics().format_events_unmirrored.load(), 0U);
}

// Mixed outputs: the mirrored event travels on video edges only; an opaque
// output that emits its own (keyframe-flagged, same-seq) packet receives that
// keyframe and nothing else. This pins the fan-out *shape* -- which edges the
// mirror reaches -- not the video_port gate; the gate's guard is
// SchedulerTest.OpaqueOutputDoesNotConsumeTheCandidate (Ruling 1: a two-leg
// recipient assertion cannot discriminate the port-kind predicate).
TEST(FormatEventTest, NonVideoEdgeReceivesTheKeyframeOnly) {
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* video_sink = nullptr;
  // "bytes" emits an opaque packet at the bound seq 7 (keyframe-flagged),
  // before "out" emits the bound video keyframe 7.
  RegisterFormatAnnouncerFixture(factory, keep, /*key_seq=*/7, /*detail_seq=*/7, &video_sink,
                                 /*keyframe=*/true, /*two_video_outputs=*/false,
                                 /*out2_seq=*/0, /*hooks=*/{},
                                 /*opaque_output=*/true, /*bytes_seq=*/7);
  EventAwareCollector* bytes_sink = nullptr;
  factory.Register(Desc("ByteCollect@1.0.0", {BytesPort("in", ge::PortDirection::kInput)}, {}),
                   [&keep, &bytes_sink](const ge::OperatorCreateArgs&) {
                     auto c = std::make_shared<EventAwareCollector>();
                     bytes_sink = c.get();
                     return Keep(keep, std::move(c));
                   });
  ge::GraphBuilder b("format-mixed-outputs");
  auto src = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto vc = b.AddNode(Op("Collect@1.0.0"), "vcollect");
  auto bc = b.AddNode(Op("ByteCollect@1.0.0"), "bcollect");
  b.Connect(src.port("out"), vc.port("in"), {.id = "e0"});
  b.Connect(src.port("bytes"), bc.port("in"), {.id = "e1"});
  auto r = ge::RuntimeTopology::Build(*b.Build(), factory, {.session_id = 11, .version = 1});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto topo = *r;
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  while (exec.RunPending()) {
  }
  ASSERT_NE(video_sink, nullptr);
  ASSERT_NE(bytes_sink, nullptr);
  const std::vector<EventAwareCollector::Entry> video_entries = video_sink->Entries();
  ASSERT_EQ(video_entries.size(), 2U) << "video edge: event then keyframe";
  EXPECT_TRUE(video_entries[0].event);
  EXPECT_EQ(video_entries[0].seq, 7U);
  EXPECT_FALSE(video_entries[1].event);
  EXPECT_EQ(video_entries[1].seq, 7U);
  const std::vector<EventAwareCollector::Entry> bytes_entries = bytes_sink->Entries();
  ASSERT_EQ(bytes_entries.size(), 1U) << "opaque edge: the keyframe only";
  EXPECT_FALSE(bytes_entries[0].event);
  EXPECT_EQ(bytes_entries[0].seq, 7U);
  EXPECT_EQ(topo->FindNode("announce")->metrics().format_events_unmirrored.load(), 0U);
}

// Review Focus #4: the same first_key_seq published twice mirrors exactly one
// event Packet per edge. The side-band contract is untouched (one publication
// per Publish call, so two here), and the shipped semantics count the
// never-mirrored duplicate as an unmatched leftover at call end.
TEST(FormatEventTest, DuplicatePublicationMirrorsOnce) {
  ObservedEvents observed;
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* sink = nullptr;
  RegisterFormatEventPair(factory, keep, &sink, /*key_seq=*/7, /*detail_seq=*/7, /*repeat=*/2);
  ge::GraphBuilder b("format-duplicate-publish");
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
  const std::vector<EventAwareCollector::Entry> entries = sink->Entries();
  ASSERT_EQ(entries.size(), 2U) << "one event Packet, not two";
  EXPECT_TRUE(entries[0].event);
  EXPECT_EQ(entries[0].seq, 7U);
  EXPECT_FALSE(entries[1].event);
  EXPECT_EQ(entries[1].seq, 7U);
  // Side-band: exactly one copy per Publish call, unchanged by the mirror.
  EXPECT_EQ(observed.Entries().size(), 2U);
  // The duplicate candidate was never mirrored, and an unmatched binding
  // must never be both lost and uncounted (Task 3, Critical 1).
  EXPECT_EQ(topo->FindNode("announce")->metrics().format_events_unmirrored.load(), 1U);
}

// A source for the mutation-race test: call 1 publishes a candidate bound to
// seq 7 and emits nothing (the candidate dies with that call's Sink and is
// counted); it then idles until |proceed|, publishes again and emits the
// bound keyframe -- after the topology swap, so the mirror must be stamped
// by the *new* topology's router.
class RepublishAnnouncer final : public ge::Operator {
 public:
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    if (stage.load() == 0) {
      Publish(req);
      stage.store(1);
      return ge::ProcessResult::kContinue;
    }
    if (!proceed.load()) return ge::ProcessResult::kContinue;
    if (stage.load() == 1) {
      Publish(req);
      stage.store(2);
      ge::Packet p;
      p.header.seq = 7;
      p.header.pts_ns = 1000;
      p.header.flags = GE_PACKET_FLAG_KEYFRAME;
      p.header.type_tag = ge::TypeTagRegistry::Global().Intern("VideoFrame");
      if (const ge::Status st = req.sink->Emit("out", std::move(p)); !st.ok()) return st;
      return ge::ProcessResult::kContinue;
    }
    return ge::ProcessResult::kExhausted;
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }
  std::atomic<int> stage{0};
  std::atomic<bool> proceed{false};

 private:
  static void Publish(const ge::ProcessRequest& req) {
    if (req.events == nullptr) return;
    ge::JsonObject d;
    d.emplace("first_key_seq", ge::JsonValue(std::uint64_t{7}));
    d.emplace("pixel_format", ge::JsonValue("NV12"));
    req.events->Publish("media_format_changed", ge::Severity::kInfo,
                        ge::JsonValue(std::move(d)));
  }
};

// Review Focus #1 + spec §顺序与失败语义 6: a candidate staged before a
// topology swap must not cross it (delivered once side-band, counted, never
// replayed onto anyone), and a publish *after* the swap must mirror through
// the new topology's PacketRouter::Prepare -- both the event and its keyframe
// carry the new topology_version and the same parameter_version; a mirror
// that bypassed Prepare would carry 0 in both fields.
// (Uses the announcer/collector shape directly rather than the shared
// fixture: the fixture's FormatAnnouncer cannot split publish and emit
// across Process calls, and this test needs one collector instance per
// topology version.)
TEST(FormatEventTest, PendingEventDoesNotCrossATopologySwap) {
  ObservedEvents observed;
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  RepublishAnnouncer* ann = nullptr;
  std::vector<EventAwareCollector*> sinks;
  factory.Register(Desc("Repub@1.0.0", {},
                        {VideoPort("out", ge::PortDirection::kOutput, {"NV12"},
                                   ge::PortCardinality::kMulti)},
                        true, 1),
                   [&keep, &ann](const ge::OperatorCreateArgs&) {
                     auto a = std::make_shared<RepublishAnnouncer>();
                     ann = a.get();
                     return Keep(keep, std::move(a));
                   });
  factory.Register(Desc("VCollect@1.0.0",
                        {VideoPort("in", ge::PortDirection::kInput, {"NV12"})}, {}),
                   [&keep, &sinks](const ge::OperatorCreateArgs&) {
                     auto c = std::make_shared<EventAwareCollector>();
                     sinks.push_back(c.get());
                     return Keep(keep, std::move(c));
                   });
  ge::GraphBuilder b1("format-swap-v1");
  auto a1 = b1.AddNode(Op("Repub@1.0.0"), "announce");
  auto c1 = b1.AddNode(Op("VCollect@1.0.0"), "collect1");
  b1.Connect(a1.port("out"), c1.port("in"), {.id = "e0"});
  auto r1 = ge::RuntimeTopology::Build(*b1.Build(), factory, {.session_id = 11, .version = 1});
  ASSERT_TRUE(r1.ok()) << r1.status().ToString();
  auto topo1 = *r1;
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo1, exec, ge::SchedulerEvents{.on_operator_event = observed.Hook()});
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  for (int i = 0; i < 1000 && ann->stage.load() < 1; ++i) (void)exec.RunOne();
  ASSERT_EQ(ann->stage.load(), 1);
  // The first call's side-band copy went out exactly once; nothing in-band;
  // the candidate died with that call and was counted.
  ASSERT_EQ(observed.Entries().size(), 1U);
  EXPECT_EQ(observed.Entries()[0].type, "media_format_changed");
  ASSERT_EQ(sinks.size(), 1U);
  EXPECT_TRUE(sinks[0]->Entries().empty());
  EXPECT_EQ(topo1->FindNode("announce")->metrics().format_events_unmirrored.load(), 1U);

  // v2 removes collect1 and adds collect2; the announcer is carried over.
  ge::GraphBuilder b2("format-swap-v2");
  auto a2 = b2.AddNode(Op("Repub@1.0.0"), "announce");
  auto c2 = b2.AddNode(Op("VCollect@1.0.0"), "collect2");
  b2.Connect(a2.port("out"), c2.port("in"), {.id = "e1"});
  ge::RuntimeTopology::BuildOptions bo;
  bo.session_id = 11;
  bo.version = 2;
  bo.base = topo1.get();
  bo.reused_nodes = {"announce"};
  auto r2 = ge::RuntimeTopology::Build(*b2.Build(), factory, bo);
  ASSERT_TRUE(r2.ok()) << r2.status().ToString();
  auto topo2 = *r2;
  ASSERT_TRUE(s.OpenNodes(*topo2).ok());
  bool drained = false;
  ge::RetireRequest retire;
  retire.policy = ge::RemovePolicy::kDrain;
  retire.drain_timeout = std::chrono::milliseconds(2000);
  retire.on_complete = [&drained](bool) { drained = true; };
  ASSERT_TRUE(s.Publish(topo2, std::move(retire)).ok());
  for (int i = 0; i < 10000 && !drained; ++i) {
    (void)exec.RunOne();
    s.Tick();
  }
  ASSERT_TRUE(drained) << "removed consumer never drained";
  ASSERT_EQ(sinks.size(), 2U);
  // The removed consumer received no event Packet (nothing at all: the
  // publish preceding the swap had no keyframe to bind to).
  EXPECT_TRUE(sinks[0]->Entries().empty());

  // A later call publishes again and emits the bound keyframe: the new
  // topology's consumer gets event(7) then keyframe(7).
  ann->proceed.store(true);
  for (int i = 0; i < 10000 && sinks[1]->Entries().size() < 2; ++i) (void)exec.RunOne();
  const std::vector<EventAwareCollector::Entry> got = sinks[1]->Entries();
  ASSERT_EQ(got.size(), 2U);
  EXPECT_TRUE(got[0].event);
  EXPECT_EQ(got[0].seq, 7U);
  EXPECT_EQ(got[0].port, "in");
  EXPECT_FALSE(got[1].event);
  EXPECT_EQ(got[1].seq, 7U);
  // Version stamps: the mirror went through PacketRouter::Prepare against
  // the same topology as its keyframe -- the new one. 0 here means the
  // mirror bypassed Prepare; 1 means it was stamped against the retired
  // topology.
  EXPECT_EQ(got[0].topology_version, 2U);
  EXPECT_EQ(got[1].topology_version, 2U);
  EXPECT_EQ(got[0].parameter_version, got[1].parameter_version);
  EXPECT_GT(got[0].parameter_version, 0U);
  // The pre-swap candidate is still the only unmirrored one, and each
  // Publish call produced exactly one side-band copy.
  EXPECT_EQ(topo1->FindNode("announce")->metrics().format_events_unmirrored.load(), 1U);
  EXPECT_EQ(observed.Entries().size(), 2U);
  while (exec.RunPending()) {
    s.Tick();
  }
  EXPECT_TRUE(s.all_closed());
}

// Review Focus #3, outside-scope half only (Task 5 owns the in-scope path).
// No .so is constructed: HostApiAdapter is exactly what PluginOperator
// drives (src/plugin_operator.cpp), built directly. With no InvokeScope
// alive, event_publish must remain the pre-change side-band call: forwarded
// exactly once with its fields intact, and no in-band Packet anywhere -- the
// recording sink stands ready and must record zero emits.
TEST(FormatEventTest, PluginPublishOutsideAScopeStaysSideBandOnly) {
  struct Captured {
    int count = 0;
    std::string type;
    ge_severity severity = GE_SEVERITY_DEBUG;
    ge_session_id session = 0;
    ge_node_id node = 0;
    std::string detail;
  } cap;
  ge::HostServices services;
  services.event_publish = [&cap](const ge_event& e) {
    ++cap.count;
    cap.type = e.type != nullptr ? e.type : "";
    cap.severity = e.severity;
    cap.session = e.session_id;
    cap.node = e.source_node_id;
    cap.detail = e.detail_json != nullptr ? e.detail_json : "";
  };
  ge::HostApiAdapter adapter(/*session=*/77, /*node=*/5, "announce", /*is_source=*/true,
                             services);
  RecordingBlockSink sink;  // stands ready; no InvokeScope ever binds it
  ge_event ev{};
  ev.header.struct_size = sizeof(ge_event);
  ev.header.abi_major = GE_ABI_MAJOR;
  ev.kind = GE_EVENT_OBSERVATION;
  ev.type = "media_format_changed";
  ev.severity = GE_SEVERITY_INFO;
  ev.session_id = 77;
  ev.source_node_id = 5;
  ev.detail_json = R"({"first_key_seq":7,"pixel_format":"NV12"})";
  const ge_status st = ge::HostApiAdapter::vtable().event_publish(&ev);
  EXPECT_EQ(st.code, GE_STATUS_OK);
  EXPECT_EQ(cap.count, 1) << "published side-band exactly once";
  EXPECT_EQ(cap.type, "media_format_changed");
  EXPECT_EQ(cap.severity, GE_SEVERITY_INFO);
  EXPECT_EQ(cap.session, 77U);
  EXPECT_EQ(cap.node, 5U);
  EXPECT_EQ(cap.detail, R"({"first_key_seq":7,"pixel_format":"NV12"})");
  EXPECT_TRUE(sink.seqs.empty()) << "an outside-scope publish must not create an in-band Packet";
}

// spec §顺序与失败语义 4: a mirrored event does not participate in input
// synchronisation. With a kAligned consumer (video + ref, both required,
// default ±40ms window) the event must not stand in for either required
// port, must not advance the alignment window past the waiting ref packet,
// and must arrive in the same single batch as its keyframe -- the collector
// counts *calls*, so an extra batch built from the event alone fails here.
TEST(FormatEventTest, MirroredEventDoesNotSatisfyASecondRequiredPort) {
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* unused = nullptr;
  RegisterFormatAnnouncerFixture(factory, keep, /*key_seq=*/7, /*detail_seq=*/7, &unused);
  auto pool = ge::HostBufferPool::Create();
  factory.Register(Desc("RefSrc@1.0.0", {},
                        {BytesPort("out", ge::PortDirection::kOutput)}, true, 1),
                   [&keep, pool](const ge::OperatorCreateArgs&) {
                     return Keep(keep, std::make_shared<CountingSource>(1, pool));
                   });
  EventAwareCollector* sink = nullptr;
  factory.Register(Desc("AlignCollect@1.0.0",
                        {VideoPort("video", ge::PortDirection::kInput, {"NV12"}),
                         BytesPort("ref", ge::PortDirection::kInput)},
                        {}),
                   [&keep, &sink](const ge::OperatorCreateArgs&) {
                     auto c = std::make_shared<EventAwareCollector>();
                     sink = c.get();
                     return Keep(keep, std::move(c));
                   });
  ge::GraphBuilder b("format-aligned");
  // refsrc first: with the inline FIFO executor its packet is on the "ref"
  // edge before the consumer's first acquire, so the aligned batch is
  // deterministic.
  auto rs = b.AddNode(Op("RefSrc@1.0.0"), "refsrc");
  auto an = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto co = b.AddNode(Op("AlignCollect@1.0.0"), "align");
  b.Connect(an.port("out"), co.port("video"), {.id = "ev", .sync = ge::SyncPolicy::kAligned});
  b.Connect(rs.port("out"), co.port("ref"), {.id = "er", .sync = ge::SyncPolicy::kAligned});
  auto r = ge::RuntimeTopology::Build(*b.Build(), factory, {.session_id = 11, .version = 1});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto topo = *r;
  ge::NodeRuntime* align = topo->FindNode("align");
  ASSERT_NE(align, nullptr);
  ASSERT_EQ(topo->InputsFor(align->id())->policy(), ge::SyncPolicy::kAligned)
      << "the negotiated policy is not kAligned: the test would not test alignment";
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  while (exec.RunPending()) {
  }
  EXPECT_TRUE(s.all_closed());
  ASSERT_NE(sink, nullptr);
  ASSERT_EQ(sink->calls.load(), 1)
      << "exactly one batch: the event alone must never produce a Process call";
  const std::vector<EventAwareCollector::Entry> got = sink->Entries();
  ASSERT_EQ(got.size(), 3U);
  EXPECT_TRUE(got[0].event);
  EXPECT_EQ(got[0].seq, 7U);
  EXPECT_EQ(got[0].port, "video");
  EXPECT_FALSE(got[1].event);
  EXPECT_EQ(got[1].seq, 7U);
  EXPECT_EQ(got[1].port, "video");
  // The ref packet was still delivered: the alignment window did not
  // advance past it on account of the event.
  EXPECT_FALSE(got[2].event);
  EXPECT_EQ(got[2].seq, 1U);
  EXPECT_EQ(got[2].port, "ref");
  EXPECT_EQ(topo->InputsFor(align->id())->late_dropped(), 0U);
}

// spec §顺序与失败语义 4, kLatest half: the mirrored event must not be the
// packet that triggers a kLatest invocation when a data packet is available.
// One call, triggered by the keyframe, with the event riding in the same
// batch ahead of it.
TEST(FormatEventTest, MirroredEventDoesNotTriggerALatestConsumer) {
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* sink = nullptr;
  RegisterFormatEventPair(factory, keep, &sink, /*key_seq=*/7, /*detail_seq=*/7);
  ge::GraphBuilder b("format-latest");
  auto src = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto col = b.AddNode(Op("Collect@1.0.0"), "collect");
  b.Connect(src.port("out"), col.port("in"), {.id = "e0", .sync = ge::SyncPolicy::kLatest});
  auto r = ge::RuntimeTopology::Build(*b.Build(), factory, {.session_id = 11, .version = 1});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto topo = *r;
  ge::NodeRuntime* col_node = topo->FindNode("collect");
  ASSERT_NE(col_node, nullptr);
  ASSERT_EQ(topo->InputsFor(col_node->id())->policy(), ge::SyncPolicy::kLatest)
      << "the negotiated policy is not kLatest: the test would not test kLatest";
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  while (exec.RunPending()) {
  }
  EXPECT_TRUE(s.all_closed());
  ASSERT_NE(sink, nullptr);
  ASSERT_EQ(sink->calls.load(), 1)
      << "one invocation, triggered by the keyframe, not by the event";
  const std::vector<EventAwareCollector::Entry> got = sink->Entries();
  ASSERT_EQ(got.size(), 2U);
  EXPECT_TRUE(got[0].event);
  EXPECT_EQ(got[0].seq, 7U);
  EXPECT_FALSE(got[1].event);
  EXPECT_EQ(got[1].seq, 7U);
}

}  // namespace
