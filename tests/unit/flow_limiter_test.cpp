#include <ge/infer/operators.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include <ge/cpp/session.h>

#include "test_operators.h"

namespace {

using namespace ge::test;

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

// Unit-level harness: drives FlowLimiter::Process directly with a recording
// sink so the admission rules are checked without a scheduler.
struct RecordingSink final : ge::EmitSink, ge::EventSink {
  ge::Status Emit(std::string_view port, ge::Packet packet) override {
    EXPECT_EQ(port, "out");
    emitted.push_back(packet.header.seq);
    return ge::Status::Ok();
  }
  void Publish(std::string_view type, ge::Severity, ge::JsonValue detail) override {
    events.emplace_back(std::string(type), std::move(detail));
  }
  std::vector<ge::PacketSeq> emitted;
  std::vector<std::pair<std::string, ge::JsonValue>> events;
};

ge::PacketRef Frame(ge::PacketSeq seq, std::int64_t pts_ns) {
  ge::Packet p;
  p.header.seq = seq;
  p.header.pts_ns = pts_ns;
  p.header.type_tag = ge::TypeTagRegistry::Global().Intern("Bytes");
  return std::make_shared<const ge::Packet>(std::move(p));
}

struct Harness {
  std::unique_ptr<ge::Operator> op;
  RecordingSink sink;
  ge::JsonValue params = ge::JsonValue(ge::JsonObject{});

  explicit Harness(ge::JsonValue options) {
    ge::OperatorCreateArgs a;
    a.key = Op("FlowLimiter@1.0.0");
    a.external_id = "lim";
    a.options = std::move(options);
    op = ge::infer::MakeFlowLimiter(a);
    ge::OpenRequest r;
    r.external_id = "lim";
    r.options = &a.options;
    EXPECT_TRUE(op->Open(r).ok());
  }
  ge::Status Feed(std::initializer_list<std::pair<const char*, ge::PacketRef>> inputs, std::uint32_t flags = 0) {
    ge::ProcessRequest req;
    req.flags = flags;
    for (const auto& [port, p] : inputs) {
      req.input_ports.emplace_back(port);
      req.inputs.push_back(p);
    }
    req.parameters = &params;
    req.sink = &sink;
    req.events = &sink;
    auto r = op->Process(req);
    return r.ok() ? ge::Status::Ok() : r.status();
  }
  ge::Status In(ge::PacketSeq seq, std::int64_t pts = 0) { return Feed({{"in", Frame(seq, pts == 0 ? static_cast<std::int64_t>(seq) : pts)}}); }
  ge::Status Finished(std::int64_t pts) { return Feed({{"finished", Frame(0, pts)}}); }
};

ge::JsonValue Opts(std::int64_t max_in_flight, const char* on_limit = "drop", std::int64_t max_held = 0,
                   std::int64_t timeout_ms = 0) {
  ge::JsonObject o;
  o["max_in_flight"] = ge::JsonValue(max_in_flight);
  o["on_limit"] = ge::JsonValue(on_limit);
  if (max_held != 0) o["max_held"] = ge::JsonValue(max_held);
  if (timeout_ms > 0) o["timeout_ms"] = ge::JsonValue(timeout_ms);
  return ge::JsonValue(std::move(o));
}

TEST(FlowLimiterTest, DropAdmitsUpToMaxInFlightThenDrops) {
  Harness h(Opts(2));
  ASSERT_TRUE(h.In(1).ok());
  ASSERT_TRUE(h.In(2).ok());
  ASSERT_TRUE(h.In(3).ok());  // dropped
  EXPECT_EQ(h.sink.emitted, (std::vector<ge::PacketSeq>{1, 2}));
  ASSERT_EQ(h.sink.events.size(), 1U);
  EXPECT_EQ(h.sink.events[0].first, "flow_limiter.drop");
  EXPECT_EQ(h.sink.events[0].second.GetInteger("seq"), 3);
  EXPECT_EQ(h.sink.events[0].second.GetInteger("in_flight"), 2);
}

TEST(FlowLimiterTest, CompletionReleasesEveryEntryUpToItsPts) {
  Harness h(Opts(2));
  ASSERT_TRUE(h.In(1, 10).ok());
  ASSERT_TRUE(h.In(2, 20).ok());
  ASSERT_TRUE(h.Finished(20).ok());  // releases pts 10 and 20
  ASSERT_TRUE(h.In(3, 30).ok());
  ASSERT_TRUE(h.In(4, 40).ok());
  ASSERT_TRUE(h.In(5, 50).ok());  // dropped
  EXPECT_EQ(h.sink.emitted, (std::vector<ge::PacketSeq>{1, 2, 3, 4}));
  ASSERT_TRUE(h.Finished(30).ok());  // releases pts 30 only
  ASSERT_TRUE(h.In(6, 60).ok());
  ASSERT_TRUE(h.In(7, 70).ok());  // dropped: 40 and 60 in flight
  EXPECT_EQ(h.sink.emitted, (std::vector<ge::PacketSeq>{1, 2, 3, 4, 6}));
}

TEST(FlowLimiterTest, CompletionWithoutPtsReleasesOldest) {
  Harness h(Opts(1));
  ASSERT_TRUE(h.In(1, 10).ok());
  ASSERT_TRUE(h.Feed({{"finished", Frame(0, 0)}}).ok());
  ASSERT_TRUE(h.In(2, 20).ok());
  EXPECT_EQ(h.sink.emitted, (std::vector<ge::PacketSeq>{1, 2}));
}

TEST(FlowLimiterTest, LatestBatchCarriesInAndFinishedTogether) {
  // Under sync "latest" a trigger on |in| brings the newest |finished|
  // along; the completion is applied before the new frame is judged.
  Harness h(Opts(1));
  ASSERT_TRUE(h.In(1, 10).ok());
  ASSERT_TRUE(h.Feed({{"in", Frame(2, 20)}, {"finished", Frame(0, 10)}}).ok());
  EXPECT_EQ(h.sink.emitted, (std::vector<ge::PacketSeq>{1, 2}));
  // Port order in the batch must not matter.
  ASSERT_TRUE(h.Feed({{"finished", Frame(0, 20)}, {"in", Frame(3, 30)}}).ok());
  EXPECT_EQ(h.sink.emitted, (std::vector<ge::PacketSeq>{1, 2, 3}));
}

TEST(FlowLimiterTest, HoldParksThenReleasesInOrderAndFlushesTheRest) {
  Harness h(Opts(1, "hold", 2));
  ASSERT_TRUE(h.In(1, 10).ok());
  ASSERT_TRUE(h.In(2, 20).ok());  // held
  ASSERT_TRUE(h.In(3, 30).ok());  // held
  ASSERT_TRUE(h.In(4, 40).ok());  // held full: oldest held (2) dropped, 4 parked
  EXPECT_EQ(h.sink.emitted, (std::vector<ge::PacketSeq>{1}));
  ASSERT_EQ(h.sink.events.size(), 1U);
  EXPECT_EQ(h.sink.events[0].second.GetInteger("seq"), 2);
  ASSERT_TRUE(h.Finished(10).ok());  // frees a slot: 3 goes out
  EXPECT_EQ(h.sink.emitted, (std::vector<ge::PacketSeq>{1, 3}));
  ASSERT_TRUE(h.Feed({}, GE_PROCESS_FLAG_FLUSH).ok());  // 4 is not lost at end of stream
  EXPECT_EQ(h.sink.emitted, (std::vector<ge::PacketSeq>{1, 3, 4}));
}

TEST(FlowLimiterTest, HotUpdateMaxInFlight) {
  Harness h(Opts(1));
  ASSERT_TRUE(h.In(1, 10).ok());
  ASSERT_TRUE(h.In(2, 20).ok());  // dropped
  h.params = ge::JsonValue(ge::JsonObject{{"max_in_flight", ge::JsonValue(3)}});
  ASSERT_TRUE(h.In(3, 30).ok());
  ASSERT_TRUE(h.In(4, 40).ok());
  ASSERT_TRUE(h.In(5, 50).ok());  // dropped: 10, 30, 40 in flight
  EXPECT_EQ(h.sink.emitted, (std::vector<ge::PacketSeq>{1, 3, 4}));
}

TEST(FlowLimiterTest, TimeoutReclaimsStaleSlots) {
  Harness h(Opts(1, "drop", 0, 20));
  ASSERT_TRUE(h.In(1, 10).ok());
  ASSERT_TRUE(h.In(2, 20).ok());  // dropped
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  ASSERT_TRUE(h.In(3, 30).ok());  // slot reclaimed, admitted
  EXPECT_EQ(h.sink.emitted, (std::vector<ge::PacketSeq>{1, 3}));
  const auto it = std::find_if(h.sink.events.begin(), h.sink.events.end(),
                               [](const auto& e) { return e.first == "flow_limiter.timeout"; });
  ASSERT_NE(it, h.sink.events.end());
  EXPECT_EQ(it->second.GetInteger("pts_ns"), 10);
}

TEST(FlowLimiterTest, RejectsBadOptions) {
  const ge::JsonValue neg_timeout(ge::JsonObject{{"max_in_flight", ge::JsonValue(1)}, {"timeout_ms", ge::JsonValue(-1)}});
  for (const ge::JsonValue& bad : {Opts(0), Opts(-3), Opts(1, "block"), Opts(1, "hold", -1), neg_timeout}) {
    ge::OperatorCreateArgs a;
    a.key = Op("FlowLimiter@1.0.0");
    a.options = bad;
    auto op = ge::infer::MakeFlowLimiter(a);
    ge::OpenRequest r;
    r.options = &a.options;
    EXPECT_FALSE(op->Open(r).ok()) << bad.Serialize();
  }
}

TEST(FlowLimiterTest, CapabilityDeclaresOptionalFeedbackAndHotParam) {
  const ge::CapabilityDescriptor d = ge::infer::FlowLimiterCapability();
  ASSERT_EQ(d.inputs.size(), 2U);
  EXPECT_TRUE(d.inputs[0].required);
  EXPECT_FALSE(d.inputs[1].required);
  EXPECT_EQ(d.inputs[1].name, "finished");
  EXPECT_EQ(d.inputs[1].type_tag, "Json");
  EXPECT_EQ(d.outputs[0].type_tag, "VideoFrame");
  EXPECT_EQ(d.parameters.hot_updatable, std::vector<std::string>{"max_in_flight"});
  EXPECT_EQ(d.inputs[0].sync->front(), ge::SyncPolicy::kLatest);
  // Round-trips through the descriptor JSON schema.
  auto parsed = ge::CapabilityDescriptor::ParseJson(d.Serialize());
  ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
  EXPECT_EQ(*parsed, d);
}

// ---------------------------------------------------------------------------
// End to end: src -> lim -> slow -> sink, with slow.done -> lim.finished as a
// feedback edge. The slow consumer acknowledges every frame it processes;
// the limiter keeps at most one frame in flight, so the consumer never sees
// a queue build up even though the source is much faster.
// ---------------------------------------------------------------------------

// Forwards |in| to |out| and reports completion on |done| (Json, pts of the
// frame). Sleeps |delay| per frame to be the bottleneck.
class SlowAcker final : public ge::Operator {
 public:
  SlowAcker(std::shared_ptr<ge::HostBufferPool> pool, std::chrono::microseconds delay)
      : pool_(std::move(pool)), delay_(delay) {}
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    for (const ge::PacketRef& in : req.inputs) {
      if (in->is_event()) continue;
      if (delay_.count() > 0) std::this_thread::sleep_for(delay_);
      ++processed;
      ge::Packet out = *in;
      if (ge::Status s = req.sink->Emit("out", std::move(out)); !s.ok() && s.code() != GE_STATUS_WOULD_BLOCK) return s;
      ge::Packet ack;
      ack.header.seq = in->header.seq;
      ack.header.pts_ns = in->header.pts_ns;
      ack.header.type_tag = ge::TypeTagRegistry::Global().Intern("Json");
      ack.payload = pool_->Allocate(2);
      std::memcpy(ack.payload->data, "{}", 2);
      if (ge::Status s = req.sink->Emit("done", std::move(ack)); !s.ok() && s.code() != GE_STATUS_WOULD_BLOCK) return s;
    }
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override {
    closed = true;
    return ge::Status::Ok();
  }
  std::atomic<int> processed{0};
  std::atomic<bool> closed{false};

 private:
  std::shared_ptr<ge::HostBufferPool> pool_;
  std::chrono::microseconds delay_;
};

ge::PortCapability JsonPort(const char* name, ge::PortDirection dir, bool required = true) {
  ge::PortCapability p = BytesPort(name, dir, required);
  p.type_tag = "Json";
  return p;
}

struct E2E {
  std::shared_ptr<ge::HostBufferPool> pool = ge::HostBufferPool::Create();
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  ge::OperationRegistry ops;
  CountingSource* src = nullptr;
  SlowAcker* slow = nullptr;
  Collector* sink = nullptr;
  std::atomic<int> drops{0};

  E2E() {
    ge::infer::RegisterFlowLimiter(factory, "Bytes", "Json");
    factory.Register(Desc("Src@1.0.0", {}, {BytesPort("out", ge::PortDirection::kOutput)}, true, 1),
                     [this](const ge::OperatorCreateArgs& a) {
                       auto s = std::make_shared<CountingSource>(a.options.GetInteger("count").value_or(100), pool);
                       src = s.get();
                       return Keep(keep, s);
                     });
    factory.Register(Desc("Slow@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput), JsonPort("done", ge::PortDirection::kOutput)},
                          true, 1),
                     [this](const ge::OperatorCreateArgs& a) {
                       auto s = std::make_shared<SlowAcker>(pool, std::chrono::microseconds(a.options.GetInteger("delay_us").value_or(0)));
                       slow = s.get();
                       return Keep(keep, s);
                     });
    factory.Register(Desc("Sink@1.0.0", {BytesPort("in", ge::PortDirection::kInput)}, {}),
                     [this](const ge::OperatorCreateArgs&) {
                       auto c = std::make_shared<Collector>();
                       sink = c.get();
                       return Keep(keep, c);
                     });
  }

  ge::GraphSpec Graph(std::int64_t count, std::int64_t max_in_flight, const char* on_limit, std::uint32_t in_capacity,
                      std::int64_t max_held = 4) {
    ge::GraphBuilder b("limited");
    auto s = b.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(count)}}));
    auto l = b.AddNode(Op("FlowLimiter@1.0.0"), "lim",
                       ge::JsonValue(ge::JsonObject{{"max_in_flight", ge::JsonValue(max_in_flight)},
                                                    {"on_limit", ge::JsonValue(on_limit)},
                                                    {"max_held", ge::JsonValue(max_held)}}));
    auto w = b.AddNode(Op("Slow@1.0.0"), "slow", ge::JsonValue(ge::JsonObject{{"delay_us", ge::JsonValue(200)}}));
    auto k = b.AddNode(Op("Sink@1.0.0"), "sink");
    ge::EdgeOptions in;
    in.id = "e0";
    in.queue.capacity = in_capacity;
    b.Connect(s.port("out"), l.port("in"), in);
    ge::EdgeOptions lim_out;
    lim_out.id = "e1";
    lim_out.queue.capacity = 8;
    b.Connect(l.port("out"), w.port("in"), lim_out);
    b.Connect(w.port("out"), k.port("in"), {.id = "e2"});
    ge::EdgeOptions fb;
    fb.id = "fb";
    fb.feedback = true;
    fb.queue.capacity = 16;
    fb.queue.policy = ge::DropPolicy::kDropOldest;
    b.Connect(w.port("done"), l.port("finished"), fb);
    auto r = b.Build();
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return *r;
  }
};

TEST(FlowLimiterE2E, DropModeBoundsConsumerBacklog) {
  E2E f;
  ge::ExecutorPool exec(2);
  ge::SessionEvents ev;
  ev.on_operator_event = [&](ge::NodeRuntime&, std::string type, ge::Severity, ge::JsonValue) {
    if (type == ge::infer::kEventFlowLimiterDrop) ++f.drops;
  };
  auto r = ge::Session::Create(f.Graph(200, 1, "drop", 64), f.factory, exec, f.ops, {}, std::move(ev));
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto& s = **r;
  ASSERT_TRUE(s.Start().ok());
  EXPECT_TRUE(s.WaitStopped(std::chrono::seconds(10)));
  // Everything that reached the slow node also reached the sink, and the
  // limiter dropped the rest instead of letting e1 fill up.
  EXPECT_EQ(f.sink->Seqs().size(), static_cast<std::size_t>(f.slow->processed.load()));
  EXPECT_GT(f.drops.load(), 0);
  EXPECT_EQ(static_cast<std::size_t>(f.slow->processed.load() + f.drops.load()), 200U);
  const ge::JsonValue snap = s.Snapshot();
  for (const ge::JsonValue& e : snap.as_object().at("edges").as_array()) {
    // At most max_in_flight (1) data packets queued, plus the EOS marker
    // that may sit behind the last admitted frame when the source ends.
    if (e.GetString("id") == "e1") {
      EXPECT_LE(e.GetInteger("max_depth").value_or(0), 2);
    }
  }
  EXPECT_TRUE(f.slow->closed);
  EXPECT_TRUE(f.sink->closed);
}

TEST(FlowLimiterE2E, HoldModeEvictsOldestHeldWhenBurstExceedsMaxHeld) {
  E2E f;
  ge::ExecutorPool exec(2);
  auto r = ge::Session::Create(f.Graph(8, 1, "hold", 2, 4), f.factory, exec, f.ops, {}, {});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto& s = **r;
  ASSERT_TRUE(s.Start().ok());
  EXPECT_TRUE(s.WaitStopped(std::chrono::seconds(10)));
  // 1 in flight, 2..4 parked, 5..8 evict them newest-wins. How many
  // completions sneak in mid-burst (each freeing a slot for one more frame)
  // is timing-dependent, so assert the invariants rather than the exact
  // set: first frame through, the newest max_held frames always survive,
  // order preserved. Eviction itself is pinned by the unit-level hold test.
  const auto got = f.sink->Seqs();
  ASSERT_GE(got.size(), 5U);
  EXPECT_EQ(got.front(), 1U);
  EXPECT_EQ(std::vector<ge::PacketSeq>(got.end() - 4, got.end()), (std::vector<ge::PacketSeq>{5, 6, 7, 8}));
  EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
}

TEST(FlowLimiterE2E, HoldModeLosesNothingWhenHeldFits) {
  E2E f;
  ge::ExecutorPool exec(2);
  // The source bursts all 8 frames before the first completion (the
  // limiter consumes |in| in O(1), so the input edge never throttles it);
  // with max_held >= burst nothing is evicted and order is preserved. The
  // held frames leave at flush because no further |in| trigger arrives
  // (sync "latest": completions are read alongside the next frame).
  auto r = ge::Session::Create(f.Graph(8, 1, "hold", 2, 8), f.factory, exec, f.ops, {}, {});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto& s = **r;
  ASSERT_TRUE(s.Start().ok());
  EXPECT_TRUE(s.WaitStopped(std::chrono::seconds(10)));
  std::vector<ge::PacketSeq> want(8);
  for (std::size_t i = 0; i < 8; ++i) want[i] = static_cast<ge::PacketSeq>(i + 1);
  EXPECT_EQ(f.sink->Seqs(), want);
}

}  // namespace
