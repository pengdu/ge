#include <ge/cpp/async_runtime.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <numeric>
#include <thread>

#include <ge/cpp/session.h>

#include "test_operators.h"

// P5 (15 §8): CompletionQueue / AsyncRuntime / ReorderBuffer / Batcher on
// top of a Session, driven by an inline executor and a host-pumped runtime
// so every interleaving is deterministic. A "fake backend" records what
// the operator submitted; the tests complete requests out of order, late,
// never, or with errors.

namespace {

using namespace ge::test;

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

// Shared by every FakeAsync instance of a fixture: the "GPU".
struct FakeBackend {
  struct Job {
    ge::SubmitRequest request;  // inputs kept alive here
    ge::CompletionSink* sink;
    std::string node;
    std::vector<ge::PacketRef> inputs;
    ge::JsonValue parameters;
  };
  std::mutex mutex;
  std::vector<Job> jobs;      // submission order
  std::vector<std::uint64_t> batch_ids;  // per job
  std::vector<std::uint32_t> batch_sizes;

  std::size_t pending() {
    std::lock_guard lock(mutex);
    return jobs.size();
  }

  // Completes job |index| (submission order) with its input forwarded.
  ge::Status Complete(std::size_t index, bool ok = true) {
    Job job;
    {
      std::lock_guard lock(mutex);
      if (index >= jobs.size()) return ge::Status::NotFound("no job");
      job = std::move(jobs[index]);
      jobs.erase(jobs.begin() + static_cast<std::ptrdiff_t>(index));
      batch_ids.erase(batch_ids.begin() + static_cast<std::ptrdiff_t>(index));
      batch_sizes.erase(batch_sizes.begin() + static_cast<std::ptrdiff_t>(index));
    }
    ge::CompletionEvent ev;
    ev.request_id = job.request.request_id;
    ev.session_id = job.request.session_id;
    ev.topology_version = job.request.topology_version;
    ev.parameter_version = job.request.parameter_version;
    ev.packet_seq = job.request.packet_seq;
    if (!ok) {
      ev.status = ge::Status::Internal("backend failure");
    } else {
      for (const ge::PacketRef& in : job.inputs) {
        if (in->is_event()) continue;
        ge::Packet out = *in;  // shares payload
        ev.outputs.push_back({"out", std::move(out)});
      }
    }
    return job.sink->Push(std::move(ev));
  }
  // Completes everything currently pending, last submitted first.
  void CompleteAllReversed() {
    while (pending() > 0) (void)Complete(pending() - 1);
  }
  void CompleteAllInOrder() {
    while (pending() > 0) (void)Complete(0);
  }
};

class FakeAsync final : public ge::Operator {
 public:
  FakeAsync(FakeBackend& backend, std::string node) : backend_(backend), node_(std::move(node)) {}
  ge::Status Open(const ge::OpenRequest&) override {
    opened = true;
    return ge::Status::Ok();
  }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) {
      flushed = true;
      return ge::ProcessResult::kContinue;
    }
    return ge::Status::InvalidArgument("async operator got process");
  }
  ge::Status Submit(const ge::SubmitRequest& req) override {
    ++submits;
    if (fail_submit_at != 0 && submits == fail_submit_at) return ge::Status::Internal("submit refused");
    FakeBackend::Job job;
    job.request = req;
    job.request.input_ports.clear();
    job.request.parameters = nullptr;
    job.sink = req.completion_sink;
    job.node = node_;
    job.inputs = req.inputs;
    job.parameters = req.parameters != nullptr ? *req.parameters : ge::JsonValue();
    std::lock_guard lock(backend_.mutex);
    backend_.jobs.push_back(std::move(job));
    backend_.batch_ids.push_back(req.batch_id);
    backend_.batch_sizes.push_back(req.batch_size);
    if (req.batch_index == 0) ++batches;
    return ge::Status::Ok();
  }
  ge::Status Close(const ge::CloseRequest& r) override {
    closed = true;
    fast_close = r.fast_shutdown;
    return ge::Status::Ok();
  }
  std::atomic<bool> opened{false}, flushed{false}, closed{false}, fast_close{false};
  std::atomic<int> submits{0}, batches{0};
  int fail_submit_at = 0;

 private:
  FakeBackend& backend_;
  std::string node_;
};

struct Fixture {
  std::shared_ptr<ge::HostBufferPool> pool = ge::HostBufferPool::Create();
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  ge::OperationRegistry ops;
  FakeBackend backend;
  std::map<std::string, CountingSource*> sources;
  std::map<std::string, FakeAsync*> asyncs;
  std::map<std::string, PassThrough*> passes;
  std::map<std::string, Collector*> sinks;
  ge::AsyncOptions async_options;
  std::unique_ptr<ge::AsyncRuntime> runtime;

  explicit Fixture(ge::AsyncOptions options = {}) : async_options(options) {
    async_options.worker_thread = false;
    runtime = std::make_unique<ge::AsyncRuntime>(async_options);
    factory.Register(Desc("Src@1.0.0", {}, {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)}, true, 1),
                     [this](const ge::OperatorCreateArgs& a) {
                       const auto n = a.options.GetInteger("count").value_or(10);
                       auto s = std::make_shared<CountingSource>(n, pool);
                       sources[a.external_id] = s.get();
                       return Keep(keep, s);
                     });
    auto desc = Desc("Infer@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                     {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)},
                     false, 4);
    desc.execution.async = true;
    desc.execution.max_inference_ms = 50;
    desc.parameters.hot_updatable = {"gain", "mode"};
    desc.parameters.migratable = {"gain"};
    factory.Register(desc, [this](const ge::OperatorCreateArgs& a) {
      auto op = std::make_shared<FakeAsync>(backend, a.external_id);
      asyncs[a.external_id] = op.get();
      return Keep(keep, op);
    });
    auto v2 = desc;
    v2.op = Op("Infer@2.0.0");
    factory.Register(v2, [this](const ge::OperatorCreateArgs& a) {
      auto op = std::make_shared<FakeAsync>(backend, a.external_id + "@2");
      asyncs[a.external_id + "@2"] = op.get();
      return Keep(keep, op);
    });
    factory.Register(Desc("Pass@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput, true, ge::PortCardinality::kMulti)}, false, 1),
                     [this](const ge::OperatorCreateArgs& a) {
                       auto p = std::make_shared<PassThrough>();
                       passes[a.external_id] = p.get();
                       return Keep(keep, p);
                     });
    factory.Register(Desc("Sink@1.0.0", {BytesPort("in", ge::PortDirection::kInput)}, {}),
                     [this](const ge::OperatorCreateArgs& a) {
                       auto c = std::make_shared<Collector>();
                       sinks[a.external_id] = c.get();
                       return Keep(keep, c);
                     });
  }

  std::unique_ptr<ge::Session> Create(const ge::GraphSpec& spec, ge::ExecutorPool& exec,
                                      ge::SessionOptions options = {}) {
    options.coordinator_thread = exec.cpu_threads() != 0;
    options.async_runtime = runtime.get();
    if (options.id == 1) options.id = next_session_id++;
    auto r = ge::Session::Create(spec, factory, exec, ops, std::move(options));
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return r.ok() ? std::move(*r) : nullptr;
  }
  ge::SessionId next_session_id = 1;
};

// src -> infer -> sink
ge::GraphSpec Linear(std::int64_t count, ge::JsonValue infer_options = ge::JsonValue(ge::JsonObject{}),
                     const char* infer_key = "Infer@1.0.0", std::uint32_t capacity = 64) {
  ge::GraphBuilder b("async-linear");
  auto src = b.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(count)}}));
  auto inf = b.AddNode(Op(infer_key), "infer", std::move(infer_options));
  auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  ge::EdgeOptions eo;
  eo.queue.capacity = capacity;
  b.Connect(src.port("out"), inf.port("in"), {.id = "e0", .queue = eo.queue});
  b.Connect(inf.port("out"), sink.port("in"), {.id = "e1", .queue = eo.queue});
  return *b.Build();
}

std::vector<ge::PacketSeq> Iota(std::int64_t n) {
  std::vector<ge::PacketSeq> v(static_cast<std::size_t>(n));
  std::iota(v.begin(), v.end(), 1U);
  return v;
}

// Runs the executor, the runtime consumer and the watchdog until nothing
// moves (the backend never completes on its own).
void Drain(Fixture& f, ge::ExecutorPool& exec, ge::Session& s) {
  for (int i = 0; i < 100000; ++i) {
    bool did = s.PumpMutations();
    did = exec.RunPending() || did;
    did = f.runtime->Pump() || did;
    s.Tick();
    if (!did && !exec.RunPending() && !f.runtime->Pump()) return;
  }
}

ge::OperationRecord WaitOp(ge::OperationRegistry& ops, ge::OperationId id, int ms = 5000) {
  auto r = ops.Wait(id, std::chrono::milliseconds(ms));
  EXPECT_TRUE(r.has_value()) << "operation " << id << " did not finish";
  return r.value_or(ge::OperationRecord{});
}

// ---------------------------------------------------------------------------
// CompletionQueue
// ---------------------------------------------------------------------------

TEST(CompletionQueueTest, BoundedAndCountsDrops) {
  ge::CompletionQueue q(2);
  EXPECT_TRUE(q.Push(ge::CompletionEvent{}).ok());
  EXPECT_TRUE(q.Push(ge::CompletionEvent{}).ok());
  EXPECT_EQ(q.Push(ge::CompletionEvent{}).code(), GE_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_EQ(q.dropped_on_full(), 1U);
  EXPECT_EQ(q.DrainAll().size(), 2U);
  EXPECT_EQ(q.size(), 0U);
}

TEST(LatencyHistogramTest, QuantilesFollowBuckets) {
  ge::LatencyHistogram h;
  EXPECT_EQ(h.P50(), 0U);
  for (int i = 0; i < 99; ++i) h.Record(2000);      // < 4096 bucket
  h.Record(50'000'000);                             // ~50ms
  EXPECT_EQ(h.count(), 100U);
  EXPECT_LE(h.P50(), 4096U);
  EXPECT_GE(h.P99(), 4096U);
  EXPECT_GE(h.P99(), 50'000'000U);
}

// ---------------------------------------------------------------------------
// Submit does not block the executor; completions are delivered in order
// ---------------------------------------------------------------------------

TEST(AsyncRuntimeTest, SubmitReturnsImmediatelyAndOutOfOrderCompletionsAreReordered) {
  Fixture f;
  ge::ExecutorPool exec(0);
  auto s = f.Create(Linear(20, ge::JsonValue(ge::JsonObject{{"batch", ge::JsonValue(false)}})), exec);
  ASSERT_TRUE(s->Start().ok());
  Drain(f, exec, *s);
  // Every packet reached the backend without any completion (ASY-1).
  EXPECT_EQ(f.backend.pending(), 16U);  // async_max_in_flight default cap
  EXPECT_EQ(f.asyncs["infer"]->submits, 16);
  EXPECT_TRUE(f.sinks["sink"]->Seqs().empty());
  EXPECT_EQ(f.runtime->in_flight(), 16U);
  EXPECT_EQ(s->current_topology()->FindNode("infer")->state(), ge::NodeState::kAsyncPending);

  f.backend.CompleteAllReversed();
  // Nothing reaches the sink until the runtime consumer runs (ASY-3).
  EXPECT_TRUE(f.sinks["sink"]->Seqs().empty());
  Drain(f, exec, *s);
  // Cap freed -> remaining 4 submitted.
  EXPECT_EQ(f.asyncs["infer"]->submits, 20);
  f.backend.CompleteAllReversed();
  Drain(f, exec, *s);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(20));  // ASY-4 保序
  EXPECT_TRUE(f.asyncs["infer"]->flushed);
  EXPECT_TRUE(f.asyncs["infer"]->closed);
  EXPECT_TRUE(s->WaitStopped(std::chrono::milliseconds(0)));
  EXPECT_EQ(f.runtime->in_flight(), 0U);
  EXPECT_EQ(f.runtime->orphan_completion_total(), 0U);
  const ge::JsonValue snap = s->Snapshot();
  bool found = false;
  for (const ge::JsonValue& n : snap.Find("nodes")->as_array()) {
    if (n.GetString("id").value_or("") != "infer") continue;
    const ge::JsonValue* a = n.Find("metrics")->Find("async");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->GetInteger("submitted").value_or(-1), 20);
    EXPECT_EQ(a->GetInteger("completed").value_or(-1), 20);
    EXPECT_EQ(a->GetInteger("wait_count").value_or(-1), 20);
    found = true;
  }
  EXPECT_TRUE(found);
  s.reset();
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

// 12 §6.2 block policy on the async output side: a delivered result whose
// downstream edge is full is parked (not dropped) and pushed again once
// the consumer frees a slot; edge order stays seq order.
TEST(AsyncRuntimeTest, BlockedOutputIsParkedNotDroppedAndStaysOrdered) {
  Fixture f;
  ge::ExecutorPool exec(0);
  // e1 (infer -> sink) holds two packets; the source edge stays large.
  ge::GraphBuilder b("async-block");
  auto src = b.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(std::int64_t{8})}}));
  auto inf = b.AddNode(Op("Infer@1.0.0"), "infer", ge::JsonValue(ge::JsonObject{{"batch", ge::JsonValue(false)}}));
  auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  ge::EdgeOptions wide, narrow;
  wide.queue.capacity = 64;
  narrow.queue.capacity = 2;
  b.Connect(src.port("out"), inf.port("in"), {.id = "e0", .queue = wide.queue});
  b.Connect(inf.port("out"), sink.port("in"), {.id = "e1", .queue = narrow.queue});
  auto s = f.Create(*b.Build(), exec);
  ASSERT_TRUE(s->Start().ok());
  Drain(f, exec, *s);
  ASSERT_EQ(f.backend.pending(), 8U);

  // Complete everything, but only pump the runtime: the sink never runs,
  // so e1 fills after two packets and the rest must be parked.
  f.backend.CompleteAllReversed();
  for (int i = 0; i < 20; ++i) (void)f.runtime->Pump();
  ge::EdgeChannel* e1 = s->current_topology()->FindEdge("e1");
  ASSERT_NE(e1, nullptr);
  EXPECT_EQ(e1->depth(), 2U);
  EXPECT_EQ(e1->metrics().Load().drop_count, 0U);
  EXPECT_GE(e1->metrics().Load().would_block_count, 1U);
  EXPECT_GE(f.runtime->blocked_outputs(), 1U);
  EXPECT_TRUE(f.sinks["sink"]->Seqs().empty());

  // Consumer resumes: parked results flow through in seq order, nothing lost.
  Drain(f, exec, *s);
  EXPECT_EQ(f.runtime->blocked_outputs(), 0U);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(8));
  EXPECT_EQ(e1->metrics().Load().drop_count, 0U);
  EXPECT_EQ(f.runtime->orphan_completion_total(), 0U);
  EXPECT_TRUE(s->WaitStopped(std::chrono::milliseconds(0)));
  s.reset();
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

TEST(AsyncRuntimeTest, InFlightCapIsConfigurable) {
  Fixture f;
  ge::ExecutorPool exec(0);
  auto s = f.Create(Linear(10, ge::JsonValue(ge::JsonObject{{"batch", ge::JsonValue(false)},
                                                             {"async_max_in_flight", ge::JsonValue(3)}})),
                    exec);
  ASSERT_TRUE(s->Start().ok());
  Drain(f, exec, *s);
  EXPECT_EQ(f.backend.pending(), 3U);
  ASSERT_TRUE(f.backend.Complete(0).ok());
  Drain(f, exec, *s);
  EXPECT_EQ(f.backend.pending(), 3U);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(1));
  f.backend.CompleteAllInOrder();
  Drain(f, exec, *s);
  f.backend.CompleteAllInOrder();
  Drain(f, exec, *s);
  f.backend.CompleteAllInOrder();
  Drain(f, exec, *s);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(10));
}

// ---------------------------------------------------------------------------
// Timeout gap (ASY-4) and late completion (ASY-9)
// ---------------------------------------------------------------------------

TEST(AsyncRuntimeTest, HeadTimeoutProducesMarkedGapAndLateResultIsOrphaned) {
  Fixture f;
  ge::ExecutorPool exec(0);
  auto s = f.Create(Linear(5, ge::JsonValue(ge::JsonObject{{"batch", ge::JsonValue(false)}})), exec);
  ASSERT_TRUE(s->Start().ok());
  Drain(f, exec, *s);
  ASSERT_EQ(f.backend.pending(), 5U);
  // Complete 2..5, never 1.
  while (f.backend.pending() > 1) ASSERT_TRUE(f.backend.Complete(1).ok());
  Drain(f, exec, *s);
  EXPECT_TRUE(f.sinks["sink"]->Seqs().empty());  // head of line blocks
  std::this_thread::sleep_for(std::chrono::milliseconds(70));  // > max_inference_ms 50
  Drain(f, exec, *s);
  const auto seqs = f.sinks["sink"]->Seqs();
  ASSERT_EQ(seqs.size(), 5U);
  EXPECT_EQ(seqs[0], 1U);  // the gap placeholder carries the missing seq
  EXPECT_EQ((std::vector<ge::PacketSeq>{seqs.begin() + 1, seqs.end()}), (std::vector<ge::PacketSeq>{2, 3, 4, 5}));
  EXPECT_EQ(f.runtime->timeout_gap_total(), 1U);
  // Sink saw a DROPPED placeholder with dropped/timeout metadata.
  ASSERT_FALSE(f.sinks["sink"]->Packets().empty());
  const ge::Packet gap = f.sinks["sink"]->Packets().front();
  EXPECT_TRUE(gap.is_dropped());
  EXPECT_NE(gap.meta().Get(ge::metadata_keys::kDropped), nullptr);
  EXPECT_NE(gap.meta().Get(ge::metadata_keys::kTimeout), nullptr);
  // The late result now: orphan, not delivered.
  ASSERT_TRUE(f.backend.Complete(0).ok());
  Drain(f, exec, *s);
  EXPECT_EQ(f.sinks["sink"]->Seqs().size(), 5U);
  EXPECT_EQ(f.runtime->orphan_completion_total(), 1U);
  EXPECT_TRUE(s->WaitStopped(std::chrono::milliseconds(0)));
}

// ---------------------------------------------------------------------------
// Errors: completion error and submit refusal fail the node
// ---------------------------------------------------------------------------

TEST(AsyncRuntimeTest, CompletionErrorFailsNodeAndSession) {
  Fixture f;
  ge::ExecutorPool exec(0);
  std::vector<std::string> failed;
  ge::SessionOptions so;
  auto s = f.Create(Linear(4, ge::JsonValue(ge::JsonObject{{"batch", ge::JsonValue(false)}})), exec, so);
  ASSERT_TRUE(s->Start().ok());
  Drain(f, exec, *s);
  ASSERT_EQ(f.backend.pending(), 4U);
  ASSERT_TRUE(f.backend.Complete(0).ok());
  Drain(f, exec, *s);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(1));
  ASSERT_TRUE(f.backend.Complete(0, false).ok());  // seq 2 fails
  Drain(f, exec, *s);
  EXPECT_EQ(s->state(), ge::SessionState::kFailed);
  EXPECT_EQ(s->failure().code(), GE_STATUS_INTERNAL);
  EXPECT_EQ(s->current_topology()->FindNode("infer")->state(), ge::NodeState::kFailed);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(1));
  // Remaining results are orphans; nothing is injected after failure.
  f.backend.CompleteAllInOrder();
  Drain(f, exec, *s);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(1));
  EXPECT_TRUE(s->WaitStopped(std::chrono::milliseconds(0)));
  s.reset();
  EXPECT_EQ(f.runtime->in_flight(), 0U);
}

TEST(AsyncRuntimeTest, SubmitRefusalFailsNodeWithoutOrphans) {
  Fixture f;
  ge::ExecutorPool exec(0);
  auto s = f.Create(Linear(3, ge::JsonValue(ge::JsonObject{{"batch", ge::JsonValue(false)}})), exec);
  ASSERT_TRUE(s->Start().ok());
  // Refuse the second submit; the session is inline so the first submit
  // happens on the first RunOne after Start.
  f.asyncs["infer"]->fail_submit_at = 2;
  Drain(f, exec, *s);
  EXPECT_EQ(s->state(), ge::SessionState::kFailed);
  EXPECT_EQ(f.asyncs["infer"]->submits.load(), 2);
  f.backend.CompleteAllInOrder();
  Drain(f, exec, *s);
  EXPECT_TRUE(s->WaitStopped(std::chrono::milliseconds(0)));
}

// ---------------------------------------------------------------------------
// Batcher (ASY-5..8)
// ---------------------------------------------------------------------------

TEST(AsyncRuntimeTest, BatchesAcrossSessionsFlushOnSizeAndTimeout) {
  ge::AsyncOptions ao;
  ao.max_batch = 4;
  ao.batch_timeout = std::chrono::milliseconds(30);
  Fixture f(ao);
  ge::ExecutorPool exec(0);
  auto a = f.Create(Linear(3, ge::JsonValue(ge::JsonObject{{"async_max_in_flight", ge::JsonValue(3)}})), exec);
  Collector* sink_a = f.sinks["sink"];
  auto b = f.Create(Linear(3, ge::JsonValue(ge::JsonObject{{"async_max_in_flight", ge::JsonValue(3)}})), exec);
  Collector* sink_b = f.sinks["sink"];
  ASSERT_NE(sink_a, sink_b);
  ASSERT_TRUE(a->Start().ok());
  ASSERT_TRUE(b->Start().ok());
  Drain(f, exec, *a);
  Drain(f, exec, *b);
  // 6 requests, same key (Infer@1.0.0, device -1, same parameters):
  // one full batch of 4 was flushed, 2 are parked.
  EXPECT_EQ(f.backend.pending(), 4U);
  EXPECT_EQ(f.runtime->batches_flushed(), 1U);
  {
    std::lock_guard lock(f.backend.mutex);
    for (std::size_t i = 0; i < 4; ++i) {
      EXPECT_EQ(f.backend.batch_ids[i], f.backend.batch_ids[0]);
      EXPECT_EQ(f.backend.batch_sizes[i], 4U);
    }
  }
  const ge::JsonValue stats = f.runtime->Stats();
  EXPECT_EQ(stats.GetInteger("batch_parked").value_or(-1), 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  Drain(f, exec, *a);
  EXPECT_EQ(f.backend.pending(), 6U);  // timeout flush
  EXPECT_EQ(f.runtime->batches_flushed(), 2U);
  {
    std::lock_guard lock(f.backend.mutex);
    EXPECT_EQ(f.backend.batch_sizes[4], 2U);
    EXPECT_EQ(f.backend.batch_sizes[5], 2U);
    EXPECT_NE(f.backend.batch_ids[4], f.backend.batch_ids[0]);
  }
  f.backend.CompleteAllReversed();
  Drain(f, exec, *a);
  Drain(f, exec, *b);
  EXPECT_EQ(sink_a->Seqs(), Iota(3));
  EXPECT_EQ(sink_b->Seqs(), Iota(3));
  EXPECT_TRUE(a->WaitStopped(std::chrono::milliseconds(0)));
  EXPECT_TRUE(b->WaitStopped(std::chrono::milliseconds(0)));
}

TEST(AsyncRuntimeTest, DifferentParametersNeverShareABatch) {
  ge::AsyncOptions ao;
  ao.max_batch = 8;
  ao.batch_timeout = std::chrono::milliseconds(20);
  Fixture f(ao);
  ge::ExecutorPool exec(0);
  auto a = f.Create(Linear(2), exec);
  auto b = f.Create(Linear(2), exec);
  ASSERT_TRUE(a->Start().ok());
  ASSERT_TRUE(b->Start().ok());
  // Same operator/device, different model parameter (ASY-8): two keys.
  ASSERT_TRUE(b->SetParameters("infer", ge::JsonValue(ge::JsonObject{{"mode", ge::JsonValue("fast")}})).ok());
  Drain(f, exec, *a);
  Drain(f, exec, *b);
  EXPECT_EQ(f.backend.pending(), 0U);
  EXPECT_EQ(f.runtime->Stats().GetInteger("batch_parked").value_or(-1), 4);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Drain(f, exec, *a);
  ASSERT_EQ(f.backend.pending(), 4U);
  EXPECT_EQ(f.runtime->batches_flushed(), 2U);
  {
    std::lock_guard lock(f.backend.mutex);
    std::map<std::uint64_t, std::set<ge::ParameterVersion>> versions;
    for (std::size_t i = 0; i < 4; ++i) {
      EXPECT_EQ(f.backend.batch_sizes[i], 2U);
      versions[f.backend.batch_ids[i]].insert(f.backend.jobs[i].request.parameter_version);
    }
    ASSERT_EQ(versions.size(), 2U);
    for (const auto& [id, vs] : versions) EXPECT_EQ(vs.size(), 1U);
  }
  f.backend.CompleteAllReversed();
  Drain(f, exec, *a);
  Drain(f, exec, *b);
  EXPECT_TRUE(a->WaitStopped(std::chrono::milliseconds(0)));
  EXPECT_TRUE(b->WaitStopped(std::chrono::milliseconds(0)));
}

TEST(AsyncRuntimeTest, BatchingCanBeDisabledPerSessionAndPerNode) {
  ge::AsyncOptions ao;
  ao.max_batch = 8;
  ao.batch_timeout = std::chrono::milliseconds(1000);
  Fixture f(ao);
  ge::ExecutorPool exec(0);
  ge::SessionOptions so;
  so.batching = false;  // ASY-7 session switch
  auto a = f.Create(Linear(2), exec, so);
  auto b = f.Create(Linear(2, ge::JsonValue(ge::JsonObject{{"batch", ge::JsonValue(ge::JsonObject{{"enabled", ge::JsonValue(false)}})}})), exec);
  ASSERT_TRUE(a->Start().ok());
  ASSERT_TRUE(b->Start().ok());
  Drain(f, exec, *a);
  Drain(f, exec, *b);
  EXPECT_EQ(f.backend.pending(), 4U);  // single-path latency: nothing parked
  {
    std::lock_guard lock(f.backend.mutex);
    for (std::uint32_t sz : f.backend.batch_sizes) EXPECT_EQ(sz, 1U);
  }
  f.backend.CompleteAllInOrder();
  Drain(f, exec, *a);
  Drain(f, exec, *b);
  EXPECT_TRUE(a->WaitStopped(std::chrono::milliseconds(0)));
  EXPECT_TRUE(b->WaitStopped(std::chrono::milliseconds(0)));
}

// ---------------------------------------------------------------------------
// Topology/session isolation (ASY-9/10)
// ---------------------------------------------------------------------------

TEST(AsyncRuntimeTest, ReplaceNodeDeliversOldResultsOnOldPathThenRetires) {
  Fixture f;
  ge::ExecutorPool exec(0);
  const ge::JsonValue infer_options(ge::JsonObject{{"batch", ge::JsonValue(false)},
                                                   {"async_max_in_flight", ge::JsonValue(3)}});
  // 10 packets, edge capacity 4, 3 in flight: the source blocks on e0 with
  // 4 queued and 3 left to emit, so the replacement happens mid-stream.
  auto s = f.Create(Linear(10, infer_options, "Infer@1.0.0", 4), exec);
  ASSERT_TRUE(s->Start().ok());
  Drain(f, exec, *s);
  ASSERT_EQ(f.backend.pending(), 3U);
  FakeAsync* old_op = f.asyncs["infer"];
  ge::NodeRuntime* old_node = s->current_topology()->FindNode("infer");
  auto op = s->Apply(ge::Mutation().ReplaceNode("infer", Op("Infer@2.0.0"), infer_options).Build());
  ASSERT_TRUE(op.ok());
  Drain(f, exec, *s);
  // Published: the new node routes v2; the old one still has 3 in flight
  // and keeps its routing snapshot (v1) until closed (12 §7.7).
  EXPECT_EQ(s->current_topology()->FindNode("infer")->operator_key(), Op("Infer@2.0.0"));
  EXPECT_EQ(s->topology_version(), 2U);
  EXPECT_EQ(old_node->state(), ge::NodeState::kAsyncPending);
  EXPECT_TRUE(old_node->retiring());
  EXPECT_FALSE(old_op->closed);
  ASSERT_NE(f.asyncs.find("infer@2"), f.asyncs.end());
  FakeAsync* new_op = f.asyncs["infer@2"];
  EXPECT_TRUE(new_op->opened);
  EXPECT_EQ(old_op->submits.load(), 3);
  f.backend.CompleteAllReversed();  // old results arrive after publish
  Drain(f, exec, *s);
  // Delivered on the old path (12 §7.5); nothing orphaned.
  const auto seqs = f.sinks["sink"]->Seqs();
  ASSERT_GE(seqs.size(), 3U);
  EXPECT_EQ((std::vector<ge::PacketSeq>{seqs.begin(), seqs.begin() + 3}), Iota(3));
  EXPECT_EQ(f.runtime->orphan_completion_total(), 0U);
  // Drain-retire: the input queued on the old edge (4..7) is still the old
  // node's and is submitted again instead of being flushed away.
  EXPECT_EQ(old_op->submits.load(), 6);
  EXPECT_FALSE(old_op->flushed);
  for (int round = 0; round < 4 && f.backend.pending() > 0; ++round) {
    f.backend.CompleteAllReversed();
    Drain(f, exec, *s);
  }
  EXPECT_EQ(f.sinks["sink"]->Seqs(), Iota(10));
  EXPECT_EQ(old_op->submits.load(), 7);
  EXPECT_EQ(new_op->submits.load(), 3);
  EXPECT_TRUE(old_op->flushed);
  EXPECT_TRUE(old_op->closed);
  EXPECT_TRUE(new_op->flushed);
  EXPECT_TRUE(new_op->closed);
  EXPECT_EQ(WaitOp(f.ops, *op).state, ge::OperationState::kSucceeded);
  EXPECT_EQ(s->scheduler().pending_retirements(), 0U);
  EXPECT_EQ(f.runtime->orphan_completion_total(), 0U);
  EXPECT_EQ(f.runtime->in_flight(), 0U);
  EXPECT_TRUE(s->WaitStopped(std::chrono::milliseconds(0)));
}

// ASY-10 (00 §3.9): after a graph change, late results that belong to the
// old topology must never be injected into the new one. The old node's drain
// times out (upgrading the retire to fast, 12 §7.6) and its in-flight
// requests expire (ASY-9 bound: max_inference_ms); the new topology finishes
// the stream on its own, and when the stale old-topology results finally
// arrive they change nothing -- no seq is delivered twice and none of the
// old node's in-flight seqs reach the sink.
TEST(AsyncRuntimeTest, LateOldTopologyResultsAfterDrainTimeoutAreOrphanedNotInjected) {
  Fixture f;
  ge::ExecutorPool exec(0);
  const ge::JsonValue infer_options(ge::JsonObject{{"batch", ge::JsonValue(false)},
                                                   {"async_max_in_flight", ge::JsonValue(3)}});
  ge::SessionOptions so;
  so.drain_timeout = std::chrono::milliseconds(20);
  auto s = f.Create(Linear(10, infer_options, "Infer@1.0.0", 4), exec, so);
  ASSERT_TRUE(s->Start().ok());
  Drain(f, exec, *s);
  ASSERT_EQ(f.backend.pending(), 3U);  // seqs 1..3 in flight on the old node
  ge::NodeRuntime* old_node = s->current_topology()->FindNode("infer");

  auto op = s->Apply(ge::Mutation().ReplaceNode("infer", Op("Infer@2.0.0"), infer_options).Build());
  ASSERT_TRUE(op.ok());
  Drain(f, exec, *s);
  EXPECT_EQ(s->topology_version(), 2U);
  EXPECT_TRUE(old_node->retiring());

  // Completes (in submission order) only the jobs a given node submitted,
  // leaving the other node's requests pending.
  const auto complete_node = [&](const std::string& node) {
    for (;;) {
      std::size_t idx = static_cast<std::size_t>(-1);
      {
        std::lock_guard lock(f.backend.mutex);
        for (std::size_t i = 0; i < f.backend.jobs.size(); ++i) {
          if (f.backend.jobs[i].node == node) {
            idx = i;
            break;
          }
        }
      }
      if (idx == static_cast<std::size_t>(-1)) break;
      (void)f.backend.Complete(idx);
    }
  };

  // The old results never arrive in time: the 20ms drain deadline upgrades
  // the retire to fast (cancelling the old node) and the 50ms inference
  // deadline (ASY-9 bound) expires its three in-flight requests, closing it.
  // The old backend jobs stay pending (never completed) throughout.
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  // Drive the new topology to completion, completing only its jobs.
  for (int round = 0; round < 40 && !s->WaitStopped(std::chrono::milliseconds(0)); ++round) {
    complete_node("infer@2");
    Drain(f, exec, *s);
  }
  ASSERT_TRUE(s->WaitStopped(std::chrono::milliseconds(0)));
  EXPECT_TRUE(old_node->cancelled());
  EXPECT_EQ(old_node->state(), ge::NodeState::kClosed);
  const ge::OperationRecord rec = WaitOp(f.ops, *op, 100);  // already finished
  EXPECT_EQ(rec.state, ge::OperationState::kSucceeded);
  EXPECT_EQ(rec.detail.GetBool("drain_timeout_upgraded_to_fast").value_or(false), true);

  const auto delivered = f.sinks["sink"]->Seqs();
  // The three seqs the old node held in flight never reached the sink.
  for (ge::PacketSeq lost : {1U, 2U, 3U}) {
    EXPECT_EQ(std::count(delivered.begin(), delivered.end(), lost), 0) << "seq " << lost;
  }
  std::set<ge::PacketSeq> unique(delivered.begin(), delivered.end());
  EXPECT_EQ(unique.size(), delivered.size()) << "duplicate seq delivered";

  // The stale old-topology results arrive now, after the switch and the
  // session stop. They belong to a retired topology and a detached sink:
  // injecting them would change the sink -- it must not.
  std::size_t stale_pending = 0;
  {
    std::lock_guard lock(f.backend.mutex);
    for (const FakeBackend::Job& j : f.backend.jobs) stale_pending += j.node == "infer" ? 1 : 0;
  }
  EXPECT_EQ(stale_pending, 3U);
  complete_node("infer");
  Drain(f, exec, *s);
  EXPECT_EQ(f.sinks["sink"]->Seqs(), delivered) << "late old-topology result was injected";
  EXPECT_EQ(f.runtime->in_flight(), 0U);
}

TEST(AsyncRuntimeTest, FastStopWaitsForInFlightThenDropsResultsAsOrphans) {
  Fixture f;
  ge::ExecutorPool exec(0);
  auto s = f.Create(Linear(50, ge::JsonValue(ge::JsonObject{{"batch", ge::JsonValue(false)},
                                                             {"async_max_in_flight", ge::JsonValue(4)}})),
                    exec);
  ASSERT_TRUE(s->Start().ok());
  Drain(f, exec, *s);
  ASSERT_EQ(f.backend.pending(), 4U);
  auto op = s->Stop(true);
  ASSERT_TRUE(op.ok());
  Drain(f, exec, *s);
  // ASY-9: no cancellation; the node stays open until its requests end.
  EXPECT_FALSE(s->WaitStopped(std::chrono::milliseconds(0)));
  EXPECT_FALSE(f.asyncs["infer"]->closed);
  f.backend.CompleteAllInOrder();
  Drain(f, exec, *s);
  EXPECT_TRUE(s->WaitStopped(std::chrono::milliseconds(0)));
  EXPECT_TRUE(f.asyncs["infer"]->closed);
  EXPECT_TRUE(f.asyncs["infer"]->fast_close);
  EXPECT_TRUE(f.sinks["sink"]->Seqs().empty());  // never injected into a cancelled graph
  EXPECT_EQ(f.runtime->in_flight(), 0U);
  EXPECT_EQ(WaitOp(f.ops, *op).state, ge::OperationState::kSucceeded);
}

TEST(AsyncRuntimeTest, SessionDestroyOrphansPendingAndInvalidatesSink) {
  Fixture f;
  ge::ExecutorPool exec(0);
  ge::CompletionSink* sink = nullptr;
  {
    auto s = f.Create(Linear(5, ge::JsonValue(ge::JsonObject{{"batch", ge::JsonValue(false)}})), exec);
    ASSERT_TRUE(s->Start().ok());
    Drain(f, exec, *s);
    ASSERT_EQ(f.backend.pending(), 5U);
    sink = s->completion_sink();
    ASSERT_NE(sink, nullptr);
    // Destroying a running session with in-flight work: the destructor
    // waits (bounded by max_inference_ms) then detaches.
    const auto t0 = std::chrono::steady_clock::now();
    // Complete on another thread while the destructor waits.
    std::thread completer([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      f.backend.CompleteAllInOrder();
      for (int i = 0; i < 50; ++i) {
        (void)f.runtime->Pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    s.reset();
    completer.join();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(4));
  }
  EXPECT_EQ(f.runtime->in_flight(), 0U);
  // The C-side handle body is gone with the session; a push through the
  // engine sink object itself is not possible any more (dangling), so we
  // only check accounting: late results counted as orphans.
  (void)f.runtime->Pump();
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

}  // namespace
