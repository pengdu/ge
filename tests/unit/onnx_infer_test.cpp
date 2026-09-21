// OnnxInfer against a real model (MobileNetV2 from the ONNX model zoo).
// Ground truth: the same tensors run through a plain single-sample
// Ort::Session in the test; the pipeline must match with batching on
// (slices stacked along dim 0 by the operator) and off.
#include <ge/infer/onnx.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <thread>

#include <ge/cpp/async_runtime.h>
#include <ge/cpp/session.h>
#include <ge/infer/tensor.h>

#include "test_operators.h"

#if GE_HAVE_ONNXRUNTIME_TEST
#include <onnxruntime_cxx_api.h>
#endif

namespace {

using namespace ge::test;

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

constexpr std::int64_t kC = 3, kH = 224, kW = 224;
constexpr std::size_t kSample = static_cast<std::size_t>(kC * kH * kW);
constexpr std::int64_t kClasses = 1000;

// Deterministic synthetic "image" i: a smooth gradient whose phase depends
// on i, so every frame yields a different logits vector.
void FillSample(std::int64_t i, float* out) {
  for (std::int64_t c = 0; c < kC; ++c) {
    for (std::int64_t y = 0; y < kH; ++y) {
      for (std::int64_t x = 0; x < kW; ++x) {
        const double v = std::sin(0.02 * static_cast<double>(x + 7 * i)) *
                             std::cos(0.03 * static_cast<double>(y + 3 * c + i)) +
                         0.1 * static_cast<double>(c);
        out[(c * kH + y) * kW + x] = static_cast<float>(v);
      }
    }
  }
}

const char* ModelPath() { return GE_MOBILENET_ONNX; }

bool HaveModel() {
  return ge::infer::OnnxRuntimeAvailable() && ModelPath()[0] != '\0' && std::ifstream(ModelPath()).good();
}

#define SKIP_WITHOUT_MODEL()                                                                     \
  if (!HaveModel()) GTEST_SKIP() << "ONNX Runtime or model not available (" << ModelPath() << ")"

// Batch window for the "actually batches" tests. The assertion is that the
// batcher merged at least two requests, which needs a second request to be
// submitted before the first one's window expires. On an idle machine 20ms
// is plenty; under the contention stress runner (8 TSan copies per core)
// the source thread can be descheduled for longer than that, so the window
// is widened there. The test still checks the batching logic, only the
// scheduler's ability to deliver two packets inside the window changes.
std::chrono::milliseconds BatchTimeout() {
  return std::chrono::milliseconds(std::getenv("GE_STRESS") != nullptr ? 500 : 20);
}

// Emits |count| Tensor packets on "out".
class TensorSource final : public ge::Operator {
 public:
  TensorSource(std::int64_t count, std::shared_ptr<ge::HostBufferPool> pool) : count_(count), pool_(std::move(pool)) {}
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    if (next_ >= count_) return ge::ProcessResult::kExhausted;
    ge::BufferRef buf = pool_->Allocate(kSample * sizeof(float));
    FillSample(next_, static_cast<float*>(buf->data));
    buf->size = kSample * sizeof(float);
    ge::infer::TensorFormat f;
    f.dtype = ge::infer::DType::kFloat32;
    f.shape = {kC, kH, kW};
    ++next_;
    ge::Packet p = ge::infer::MakeTensorPacket(std::move(buf), f, static_cast<ge::PacketSeq>(next_), next_ * 33'000'000);
    ge::Status s = req.sink->Emit("out", std::move(p));
    if (s.code() == GE_STATUS_WOULD_BLOCK) --next_;
    else if (!s.ok()) return s;
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }

 private:
  std::int64_t count_;
  std::shared_ptr<ge::HostBufferPool> pool_;
  std::int64_t next_ = 0;
};

// Collects Tensor packets: seq -> logits.
class LogitsSink final : public ge::Operator {
 public:
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    std::lock_guard lock(mutex_);
    for (const ge::PacketRef& p : req.inputs) {
      if (p->is_event()) continue;
      auto f = ge::infer::FormatOf(*p);
      if (!f.ok()) return f.status();
      formats[p->header.seq] = *f;
      std::vector<float> v(f->ElementCount());
      std::memcpy(v.data(), p->payload->data, p->payload->size);
      logits[p->header.seq] = std::move(v);
      order.push_back(p->header.seq);
    }
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override {
    closed = true;
    return ge::Status::Ok();
  }
  std::map<ge::PacketSeq, std::vector<float>> Logits() const {
    std::lock_guard lock(mutex_);
    return logits;
  }
  std::vector<ge::PacketSeq> Order() const {
    std::lock_guard lock(mutex_);
    return order;
  }
  std::map<ge::PacketSeq, ge::infer::TensorFormat> formats;
  std::atomic<bool> closed{false};

 private:
  mutable std::mutex mutex_;
  std::map<ge::PacketSeq, std::vector<float>> logits;
  std::vector<ge::PacketSeq> order;
};

#if GE_HAVE_ONNXRUNTIME_TEST
// Reference: one sample at a time through ORT directly.
std::map<ge::PacketSeq, std::vector<float>> Reference(std::int64_t count) {
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ref");
  Ort::SessionOptions so;
  so.SetIntraOpNumThreads(1);
  Ort::Session session(env, ModelPath(), so);
  Ort::AllocatorWithDefaultOptions alloc;
  const auto in_name = session.GetInputNameAllocated(0, alloc);
  const auto out_name = session.GetOutputNameAllocated(0, alloc);
  const char* ins[] = {in_name.get()};
  const char* outs[] = {out_name.get()};
  std::map<ge::PacketSeq, std::vector<float>> ref;
  std::vector<float> sample(kSample);
  const std::int64_t dims[] = {1, kC, kH, kW};
  for (std::int64_t i = 0; i < count; ++i) {
    FillSample(i, sample.data());
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value in = Ort::Value::CreateTensor<float>(mem, sample.data(), sample.size(), dims, 4);
    auto r = session.Run(Ort::RunOptions{nullptr}, ins, &in, 1, outs, 1);
    const float* d = r[0].GetTensorData<float>();
    ref[static_cast<ge::PacketSeq>(i + 1)] = std::vector<float>(d, d + kClasses);
  }
  return ref;
}
#else
std::map<ge::PacketSeq, std::vector<float>> Reference(std::int64_t) { return {}; }
#endif

ge::PacketSeq ArgMax(const std::vector<float>& v) {
  return static_cast<ge::PacketSeq>(std::max_element(v.begin(), v.end()) - v.begin());
}

struct Fixture {
  std::shared_ptr<ge::HostBufferPool> pool = ge::HostBufferPool::Create();
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  ge::OperationRegistry ops;
  std::unique_ptr<ge::AsyncRuntime> runtime;
  std::map<std::string, LogitsSink*> sinks;

  explicit Fixture(ge::AsyncOptions ao = {}) {
    runtime = std::make_unique<ge::AsyncRuntime>(ao);
    ge::infer::RegisterOnnxInfer(factory);
    ge::PortCapability out;
    out.name = "out";
    out.direction = ge::PortDirection::kOutput;
    out.type_tag = "Tensor";
    out.cardinality = ge::PortCardinality::kMulti;
    factory.Register(Desc("TensorSrc@1.0.0", {}, {out}, true, 1), [this](const ge::OperatorCreateArgs& a) {
      auto s = std::make_shared<TensorSource>(a.options.GetInteger("count").value_or(8), pool);
      return Keep(keep, s);
    });
    ge::PortCapability in;
    in.name = "in";
    in.direction = ge::PortDirection::kInput;
    in.type_tag = "Tensor";
    factory.Register(Desc("LogitsSink@1.0.0", {in}, {}), [this](const ge::OperatorCreateArgs& a) {
      auto c = std::make_shared<LogitsSink>();
      // Keyed by session so two sessions with the same node ids stay apart.
      sinks[std::to_string(a.session_id) + "/" + a.external_id] = c.get();
      return Keep(keep, c);
    });
  }

  ge::GraphSpec Graph(std::int64_t count, ge::JsonObject infer_opts = {}, const char* name = "infer") {
    infer_opts["model"] = ge::JsonValue(std::string(ModelPath()));
    ge::GraphBuilder b(name);
    auto s = b.AddNode(Op("TensorSrc@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(count)}}));
    auto n = b.AddNode(Op("OnnxInfer@1.0.0"), "net", ge::JsonValue(std::move(infer_opts)));
    auto k = b.AddNode(Op("LogitsSink@1.0.0"), "sink");
    b.Connect(s.port("out"), n.port("in"), {.id = "e0"});
    b.Connect(n.port("out"), k.port("in"), {.id = "e1"});
    auto r = b.Build();
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return *r;
  }

  std::unique_ptr<ge::Session> Create(const ge::GraphSpec& spec, ge::ExecutorPool& exec, ge::SessionOptions o = {}) {
    o.async_runtime = runtime.get();
    auto r = ge::Session::Create(spec, factory, exec, ops, std::move(o));
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return r.ok() ? std::move(*r) : nullptr;
  }
};

void ExpectClose(const std::map<ge::PacketSeq, std::vector<float>>& got,
                 const std::map<ge::PacketSeq, std::vector<float>>& ref) {
  ASSERT_EQ(got.size(), ref.size());
  for (const auto& [seq, r] : ref) {
    const auto it = got.find(seq);
    ASSERT_NE(it, got.end()) << "seq " << seq << " missing";
    ASSERT_EQ(it->second.size(), r.size());
    EXPECT_EQ(ArgMax(it->second), ArgMax(r)) << "seq " << seq;
    float max_diff = 0;
    for (std::size_t i = 0; i < r.size(); ++i) max_diff = std::max(max_diff, std::fabs(it->second[i] - r[i]));
    // Batched vs single-sample execution may differ by kernel choice; the
    // tolerance is well below the logit spread of distinct inputs.
    EXPECT_LT(max_diff, 1e-3f) << "seq " << seq;
  }
}

TEST(OnnxInferTest, CapabilityIsAsyncTensorToTensor) {
  const ge::CapabilityDescriptor d = ge::infer::OnnxInferCapability();
  EXPECT_TRUE(d.execution.async);
  EXPECT_TRUE(d.execution.max_inference_ms.has_value());
  EXPECT_EQ(d.inputs[0].type_tag, "Tensor");
  EXPECT_EQ(d.outputs[0].type_tag, "Tensor");
  auto parsed = ge::CapabilityDescriptor::ParseJson(d.Serialize());
  ASSERT_TRUE(parsed.ok()) << parsed.status().ToString();
  EXPECT_EQ(*parsed, d);
}

TEST(OnnxInferTest, OpenRejectsMissingOrBadModel) {
  if (!ge::infer::OnnxRuntimeAvailable()) GTEST_SKIP();
  for (const ge::JsonValue& opts : {ge::JsonValue(ge::JsonObject{}),
                                    ge::JsonValue(ge::JsonObject{{"model", ge::JsonValue("/nonexistent/x.onnx")}})}) {
    ge::OperatorCreateArgs a;
    a.key = Op("OnnxInfer@1.0.0");
    a.options = opts;
    auto op = ge::infer::MakeOnnxInfer(a);
    ge::OpenRequest r;
    r.external_id = "net";
    r.options = &a.options;
    EXPECT_FALSE(op->Open(r).ok()) << opts.Serialize();
  }
}

TEST(OnnxInferTest, UnbatchedMatchesReference) {
  SKIP_WITHOUT_MODEL();
  ge::AsyncOptions ao;
  ao.batching = false;
  Fixture f(ao);
  ge::ExecutorPool exec(2);
  auto s = f.Create(f.Graph(6), exec);
  {
    ge::Status st = s->Start();
    ASSERT_TRUE(st.ok()) << st.ToString();
  }
  ASSERT_TRUE(s->WaitStopped(std::chrono::seconds(120)));
  ExpectClose(f.sinks["1/sink"]->Logits(), Reference(6));
  const auto order = f.sinks["1/sink"]->Order();
  EXPECT_TRUE(std::is_sorted(order.begin(), order.end()));  // ReorderBuffer kept seq order
  EXPECT_EQ(f.sinks["1/sink"]->formats.at(1).shape, std::vector<std::int64_t>{kClasses});
  EXPECT_TRUE(f.sinks["1/sink"]->closed);
  EXPECT_EQ(f.runtime->orphan_completion_total(), 0U);
}

TEST(OnnxInferTest, BatchedMatchesReferenceAndActuallyBatches) {
  SKIP_WITHOUT_MODEL();
  ge::AsyncOptions ao;
  ao.batching = true;
  ao.max_batch = 4;
  ao.batch_timeout = BatchTimeout();
  Fixture f(ao);
  ge::ExecutorPool exec(2);
  // A small simulated delay keeps requests queued long enough to form
  // batches without depending on the source/inference speed ratio.
  auto s = f.Create(f.Graph(12, {{"simulate_delay_ms", ge::JsonValue(5)}}), exec);
  ASSERT_TRUE(s->Start().ok());
  ASSERT_TRUE(s->WaitStopped(std::chrono::seconds(120)));
  ExpectClose(f.sinks["1/sink"]->Logits(), Reference(12));
  const auto order = f.sinks["1/sink"]->Order();
  EXPECT_TRUE(std::is_sorted(order.begin(), order.end()));
  const ge::JsonValue snap = s->Snapshot();
  for (const ge::JsonValue& n : snap.as_object().at("nodes").as_array()) {
    if (n.GetString("id") != "net") continue;
    const ge::JsonValue& a = n.as_object().at("metrics").as_object().at("async");
    EXPECT_EQ(a.GetInteger("completed"), 12);
    EXPECT_EQ(a.GetInteger("orphan_completions"), 0);
    // batch_size_sum / batch_count > 1 means at least one multi-member batch
    // reached Submit; the operator stacked it along dim 0.
    EXPECT_GT(a.GetInteger("batch_size_sum").value_or(0), a.GetInteger("batch_count").value_or(0));
  }
  EXPECT_GT(f.runtime->batches_flushed(), 0U);
  EXPECT_LT(f.runtime->batches_flushed(), 12U);
}

TEST(OnnxInferTest, TwoSessionsShareTheBatcher) {
  SKIP_WITHOUT_MODEL();
  ge::AsyncOptions ao;
  ao.max_batch = 8;
  ao.batch_timeout = BatchTimeout();
  Fixture f(ao);
  ge::ExecutorPool exec(4);
  auto s1 = f.Create(f.Graph(6, {{"simulate_delay_ms", ge::JsonValue(5)}}, "a"), exec, {.id = 1});
  auto s2 = f.Create(f.Graph(6, {{"simulate_delay_ms", ge::JsonValue(5)}}, "b"), exec, {.id = 2});
  ASSERT_TRUE(s1->Start().ok());
  ASSERT_TRUE(s2->Start().ok());
  ASSERT_TRUE(s1->WaitStopped(std::chrono::seconds(120)));
  ASSERT_TRUE(s2->WaitStopped(std::chrono::seconds(120)));
  const auto ref = Reference(6);
  // Each session got its own results (no cross-session slice mix-up).
  ASSERT_EQ(f.sinks.size(), 2U);
  for (auto& [id, sink] : f.sinks) ExpectClose(sink->Logits(), ref);
  EXPECT_EQ(f.runtime->orphan_completion_total(), 0U);
  // 12 requests total; fewer flushes than that means some batch spanned
  // requests, and with both sessions submitting concurrently on the same
  // model key some of those batches mixed sessions.
  EXPECT_LT(f.runtime->batches_flushed(), 12U);
}

TEST(OnnxInferTest, FastStopWithInFlightLeavesNoOrphans) {
  SKIP_WITHOUT_MODEL();
  ge::AsyncOptions ao;
  ao.batching = false;
  Fixture f(ao);
  ge::ExecutorPool exec(2);
  auto s = f.Create(f.Graph(200, {{"simulate_delay_ms", ge::JsonValue(30)}}), exec);
  ASSERT_TRUE(s->Start().ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  auto op = s->Stop(true);
  ASSERT_TRUE(op.ok());
  ASSERT_TRUE(s->WaitStopped(std::chrono::seconds(30)));
  // Whatever did complete is correct; nothing crashed; the sink outlived
  // every completion (13 §7.3) so late ones are counted, not delivered.
  const auto got = f.sinks["1/sink"]->Logits();
  const auto ref = Reference(static_cast<std::int64_t>(got.empty() ? 1 : got.rbegin()->first));
  for (const auto& [seq, v] : got) EXPECT_EQ(ArgMax(v), ArgMax(ref.at(seq))) << seq;
  EXPECT_LT(got.size(), 200U);
  EXPECT_EQ(f.runtime->in_flight(), 0U);
}

TEST(OnnxInferTest, ShapeMismatchFailsTheNodeNotTheEngine) {
  SKIP_WITHOUT_MODEL();
  Fixture f;
  // Producer with the wrong tensor shape (H=W=100).
  ge::PortCapability out;
  out.name = "out";
  out.direction = ge::PortDirection::kOutput;
  out.type_tag = "Tensor";
  class BadSrc final : public ge::Operator {
   public:
    explicit BadSrc(std::shared_ptr<ge::HostBufferPool> p) : pool_(std::move(p)) {}
    ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
    ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
      if (req.flags & GE_PROCESS_FLAG_FLUSH || done_) return done_ ? ge::ProcessResult::kExhausted : ge::ProcessResult::kContinue;
      done_ = true;
      ge::infer::TensorFormat fmt;
      fmt.shape = {3, 100, 100};
      ge::BufferRef b = pool_->Allocate(fmt.ByteSize());
      b->size = fmt.ByteSize();
      return req.sink->Emit("out", ge::infer::MakeTensorPacket(std::move(b), fmt, 1, 0)).ok()
                 ? ge::ProcessResult::kContinue
                 : ge::ProcessResult::kExhausted;
    }
    ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }

   private:
    std::shared_ptr<ge::HostBufferPool> pool_;
    bool done_ = false;
  };
  f.factory.Register(Desc("BadSrc@1.0.0", {}, {out}, true, 1),
                     [&](const ge::OperatorCreateArgs&) { return Keep(f.keep, std::make_shared<BadSrc>(f.pool)); });
  ge::GraphBuilder b("bad");
  auto s = b.AddNode(Op("BadSrc@1.0.0"), "src");
  auto n = b.AddNode(Op("OnnxInfer@1.0.0"), "net", ge::JsonValue(ge::JsonObject{{"model", ge::JsonValue(std::string(ModelPath()))}}));
  auto k = b.AddNode(Op("LogitsSink@1.0.0"), "sink");
  b.Connect(s.port("out"), n.port("in"));
  b.Connect(n.port("out"), k.port("in"));
  ge::ExecutorPool exec(2);
  std::atomic<int> failed{0};
  ge::SessionEvents ev;
  ev.on_node_failed = [&](ge::NodeRuntime& node, const ge::Status& st) {
    if (node.external_id() == "net") {
      ++failed;
      EXPECT_NE(st.message().find("does not match model"), std::string::npos) << st.ToString();
    }
  };
  ge::SessionOptions so;
  so.async_runtime = f.runtime.get();
  auto r = ge::Session::Create(*b.Build(), f.factory, exec, f.ops, so, std::move(ev));
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  ASSERT_TRUE((*r)->Start().ok());
  ASSERT_TRUE((*r)->WaitStopped(std::chrono::seconds(30)));
  EXPECT_EQ(failed.load(), 1);
  EXPECT_TRUE(f.sinks["1/sink"]->Logits().empty());
}

}  // namespace
