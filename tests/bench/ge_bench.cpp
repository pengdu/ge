// Performance baselines for 15 §4 (P1) and §5 (P2). Not a gtest: prints a
// small JSON report and exits non-zero only when a hard budget is
// exceeded. Run on an optimised build; sanitizer builds skip the budgets.
//
//   ge_bench --iterations N --packets M [--threads T] [--json]
//
// Metrics
//   validate_100n_300e_ms   P1: negotiate+validate a 100 Node / 300 Edge
//                           graph (<50ms budget)
//   pipeline_latency_us     P2: end-to-end framework latency through a
//                           10 Node linear graph on a threaded executor,
//                           one packet in flight (ping-pong), source stamp
//                           -> sink arrival (<50us budget, p50)
//   schedule_overhead_us    P2: per-node scheduling cost, inline executor:
//                           wall time / (packets x nodes) (<2us budget)

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <algorithm>
#include <atomic>
#include <map>

#include <ge/cpp/graph_spec.h>
#include <ge/cpp/graph_validator.h>
#include <ge/cpp/operator.h>
#include <ge/cpp/session.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || __has_feature(address_sanitizer) || \
    __has_feature(thread_sanitizer)
#define GE_BENCH_SANITIZED 1
#else
#define GE_BENCH_SANITIZED 0
#endif

// The budgets below are wall-clock p50s on the reference machine and only mean
// anything for an optimized build. An unoptimized (-O0, the default
// CMAKE_BUILD_TYPE here) run is 2-5x off the numbers the budgets were set from,
// so it reports the measurements and skips the verdict instead of failing on
// the build's own overhead. Same rationale as the sanitizer gate above.
#if defined(__OPTIMIZE__) || defined(_MSC_VER)
#define GE_BENCH_OPTIMIZED 1
#else
#define GE_BENCH_OPTIMIZED 0
#endif

namespace {

using Clock = std::chrono::steady_clock;

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const auto idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
  return v[idx];
}

// ---------------------------------------------------------------------------
// P1: 100 Node / 300 Edge validation
// ---------------------------------------------------------------------------

ge::PortCapability Port(const char* name, ge::PortDirection dir, std::vector<std::string> pf) {
  ge::PortCapability p;
  p.name = name;
  p.direction = dir;
  p.type_tag = "VideoFrame";
  p.video = ge::VideoConstraints{};
  p.video->pixel_formats = std::move(pf);
  return p;
}

struct ValidateBench {
  std::map<ge::OperatorKey, ge::CapabilityDescriptor> descs;
  ge::GraphSpec spec{"big"};

  ValidateBench() {
    ge::CapabilityDescriptor src;
    src.op = Op("Src@1.0.0");
    src.outputs = {Port("out", ge::PortDirection::kOutput, {"NV12", "P010"})};
    src.outputs[0].cardinality = ge::PortCardinality::kMulti;
    src.execution.max_parallelism = 4;
    descs[src.op] = src;
    ge::CapabilityDescriptor mix;
    mix.op = Op("Mix@1.0.0");
    mix.inputs = {Port("a", ge::PortDirection::kInput, {"NV12"}), Port("b", ge::PortDirection::kInput, {"NV12"})};
    mix.inputs[1].required = false;
    mix.inputs[1].cardinality = ge::PortCardinality::kMulti;
    mix.outputs = {Port("out", ge::PortDirection::kOutput, {"NV12"})};
    mix.outputs[0].cardinality = ge::PortCardinality::kMulti;
    mix.execution.max_parallelism = 4;
    descs[mix.op] = mix;

    (void)spec.AddNode({.id = "src", .op = Op("Src@1.0.0")});
    for (int i = 0; i < 99; ++i) (void)spec.AddNode({.id = "mix" + std::to_string(i), .op = Op("Mix@1.0.0")});
    int edges = 0;
    for (int i = 0; i < 99; ++i) {
      const std::string from = i == 0 ? "src" : "mix" + std::to_string(i - 1);
      (void)spec.AddEdge({.from = {from, "out"}, .to = {"mix" + std::to_string(i), "a"}});
      ++edges;
    }
    for (int i = 1; i < 99 && edges < 300; ++i) {
      for (int j = i - 1; j >= 0 && edges < 300 && j > i - 4; --j) {
        (void)spec.AddEdge({.from = {"mix" + std::to_string(j), "out"}, .to = {"mix" + std::to_string(i), "b"}});
        ++edges;
      }
    }
  }

  double RunOnceMs() {
    ge::GraphValidator v([this](const ge::OperatorKey& k) -> const ge::CapabilityDescriptor* {
      const auto it = descs.find(k);
      return it == descs.end() ? nullptr : &it->second;
    });
    const auto t0 = Clock::now();
    const auto r = v.Validate(spec);
    const auto t1 = Clock::now();
    if (!r.ok()) {
      std::fprintf(stderr, "validate failed: %s\n", r.status().ToString().c_str());
      std::exit(2);
    }
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
  }
};

// ---------------------------------------------------------------------------
// P2: 10 Node linear pipeline
// ---------------------------------------------------------------------------

// Stamps the emit time into the payload so the sink can measure latency.
// In ping-pong mode the next packet is emitted only after the previous one
// reached the sink, so the pipeline is unloaded and the sample is pure
// framework latency (no queueing).
class StampSource final : public ge::Operator {
 public:
  StampSource(std::int64_t count, std::shared_ptr<ge::HostBufferPool> pool,
              const std::atomic<std::int64_t>* delivered)
      : count_(count), pool_(std::move(pool)), delivered_(delivered) {}
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    if (next_ >= count_) return ge::ProcessResult::kExhausted;
    if (delivered_ != nullptr && delivered_->load(std::memory_order_acquire) < next_) {
      std::this_thread::yield();  // previous packet still in flight
      return ge::ProcessResult::kContinue;
    }
    ge::Packet p;
    p.header.seq = static_cast<ge::PacketSeq>(next_ + 1);
    p.header.type_tag = ge::TypeTagRegistry::Global().Intern("Bytes");
    p.payload = pool_->Allocate(sizeof(std::int64_t));
    const std::int64_t now = Clock::now().time_since_epoch().count();
    std::memcpy(p.payload->data, &now, sizeof(now));
    const ge::Status s = req.sink->Emit("out", std::move(p));
    if (s.code() == GE_STATUS_WOULD_BLOCK) return ge::ProcessResult::kContinue;  // retry same seq
    if (!s.ok()) return s;
    ++next_;
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }

 private:
  std::int64_t count_;
  std::shared_ptr<ge::HostBufferPool> pool_;
  const std::atomic<std::int64_t>* delivered_;
  std::int64_t next_ = 0;
};

class Forward final : public ge::Operator {
 public:
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    for (const ge::PacketRef& in : req.inputs) {
      if (in->is_event()) continue;
      ge::Packet out = *in;
      const ge::Status s = req.sink->Emit("out", std::move(out));
      if (!s.ok() && s.code() != GE_STATUS_WOULD_BLOCK) return s;
    }
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }
};

class LatencySink final : public ge::Operator {
 public:
  explicit LatencySink(std::atomic<std::int64_t>* gate) : gate_(gate) {}
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    const std::int64_t now = Clock::now().time_since_epoch().count();
    std::lock_guard lock(mutex_);
    for (const ge::PacketRef& in : req.inputs) {
      if (in->is_event() || !in->payload) continue;
      std::int64_t stamp = 0;
      std::memcpy(&stamp, in->payload->data, sizeof(stamp));
      latencies_ns.push_back(static_cast<double>(now - stamp));
      gate_->fetch_add(1, std::memory_order_acq_rel);
    }
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }
  std::vector<double> Latencies() const {
    std::lock_guard lock(mutex_);
    return latencies_ns;
  }

 private:
  std::atomic<std::int64_t>* gate_;
  mutable std::mutex mutex_;
  std::vector<double> latencies_ns;
};

ge::PortCapability BytesPort(const char* name, ge::PortDirection dir) {
  ge::PortCapability p;
  p.name = name;
  p.direction = dir;
  p.type_tag = "Bytes";
  p.cardinality = dir == ge::PortDirection::kOutput ? ge::PortCardinality::kMulti : ge::PortCardinality::kSingle;
  return p;
}

ge::CapabilityDescriptor Desc(const char* key, std::vector<ge::PortCapability> ins,
                              std::vector<ge::PortCapability> outs, bool stateful) {
  ge::CapabilityDescriptor d;
  d.op = Op(key);
  d.inputs = std::move(ins);
  d.outputs = std::move(outs);
  d.execution.stateful = stateful;
  d.execution.max_parallelism = 1;
  return d;
}

constexpr int kPipelineNodes = 10;  // src + 8 forwards + sink

struct PipelineBench {
  std::shared_ptr<ge::HostBufferPool> pool = ge::HostBufferPool::Create();
  ge::BuiltinOperatorFactory factory;
  ge::OperationRegistry ops;
  LatencySink* sink = nullptr;
  std::int64_t packets;
  bool ping_pong;

  PipelineBench(std::int64_t count, bool ping_pong_mode) : packets(count), ping_pong(ping_pong_mode) {
    factory.Register(Desc("Src@1.0.0", {}, {BytesPort("out", ge::PortDirection::kOutput)}, true),
                     [this](const ge::OperatorCreateArgs&) {
                       // The sink is created before the source (topological
                       // order is src first, so the pointer is resolved lazily).
                       return std::make_unique<StampSource>(packets, pool, ping_pong ? &gate : nullptr);
                     });
    factory.Register(Desc("Fwd@1.0.0", {BytesPort("in", ge::PortDirection::kInput)},
                          {BytesPort("out", ge::PortDirection::kOutput)}, false),
                     [](const ge::OperatorCreateArgs&) { return std::make_unique<Forward>(); });
    factory.Register(Desc("Sink@1.0.0", {BytesPort("in", ge::PortDirection::kInput)}, {}, false),
                     [this](const ge::OperatorCreateArgs&) {
                       auto s = std::make_unique<LatencySink>(&gate);
                       sink = s.get();
                       return s;
                     });
  }
  std::atomic<std::int64_t> gate{0};  // packets delivered to the sink

  static ge::GraphSpec Graph() {
    ge::GraphBuilder b("bench-linear");
    ge::EdgeOptions eo;
    eo.queue.capacity = 256;
    auto prev = b.AddNode(Op("Src@1.0.0"), "src");
    for (int i = 0; i < kPipelineNodes - 2; ++i) {
      auto n = b.AddNode(Op("Fwd@1.0.0"), "fwd" + std::to_string(i));
      b.Connect(prev.port("out"), n.port("in"), {.id = "e" + std::to_string(i), .queue = eo.queue});
      prev = n;
    }
    auto s = b.AddNode(Op("Sink@1.0.0"), "sink");
    b.Connect(prev.port("out"), s.port("in"), {.id = "e_sink", .queue = eo.queue});
    return *b.Build();
  }

  // Threaded: end-to-end latency samples (ns).
  std::vector<double> RunThreaded(std::uint32_t threads) {
    ge::ExecutorPool exec(threads);
    ge::SessionOptions so;
    so.coordinator_thread = true;
    auto r = ge::Session::Create(Graph(), factory, exec, ops, std::move(so));
    if (!r.ok()) {
      std::fprintf(stderr, "session: %s\n", r.status().ToString().c_str());
      std::exit(2);
    }
    auto& s = **r;
    gate.store(0);
    if (const ge::Status st = s.Start(); !st.ok()) {
      std::fprintf(stderr, "start: %s\n", st.ToString().c_str());
      std::exit(2);
    }
    if (!s.WaitStopped(std::chrono::seconds(60))) {
      std::fprintf(stderr, "pipeline did not finish\n");
      std::exit(2);
    }
    return sink->Latencies();
  }

  // Inline: wall time per (packet x node) in ns.
  double RunInlineOverheadNs() {
    ge::ExecutorPool exec(0);
    ge::SessionOptions so;
    so.coordinator_thread = false;
    auto r = ge::Session::Create(Graph(), factory, exec, ops, std::move(so));
    if (!r.ok()) {
      std::fprintf(stderr, "session: %s\n", r.status().ToString().c_str());
      std::exit(2);
    }
    auto& s = **r;
    gate.store(0);
    if (const ge::Status st = s.Start(); !st.ok()) {
      std::fprintf(stderr, "start: %s\n", st.ToString().c_str());
      std::exit(2);
    }
    const auto t0 = Clock::now();
    for (int i = 0; i < 100'000'000; ++i) {
      bool did = s.PumpMutations();
      did = exec.RunPending() || did;
      s.Tick();
      if (!did && s.WaitStopped(std::chrono::milliseconds(0))) break;
    }
    const auto t1 = Clock::now();
    const double total_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return total_ns / (static_cast<double>(packets) * kPipelineNodes);
  }
};

}  // namespace

int main(int argc, char** argv) {
  int iterations = 20;
  std::int64_t packets = 20000;
  std::uint32_t threads = 4;
  bool json = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--iterations") iterations = std::stoi(next());
    else if (a == "--packets") packets = std::stoll(next());
    else if (a == "--threads") threads = static_cast<std::uint32_t>(std::stoul(next()));
    else if (a == "--json") json = true;
    else {
      std::fprintf(stderr, "usage: %s [--iterations N] [--packets M] [--threads T] [--json]\n", argv[0]);
      return 2;
    }
  }

  ValidateBench vb;
  std::vector<double> validate_ms;
  (void)vb.RunOnceMs();  // warm-up
  for (int i = 0; i < iterations; ++i) validate_ms.push_back(vb.RunOnceMs());

  std::vector<double> latency_us;
  const std::int64_t latency_packets = std::min<std::int64_t>(packets, 2000);
  {
    PipelineBench pb(latency_packets, true);
    (void)pb.RunThreaded(threads);  // warm-up
    for (int i = 0; i < std::max(1, iterations / 4); ++i) {
      for (double ns : pb.RunThreaded(threads)) latency_us.push_back(ns / 1000.0);
    }
  }
  std::vector<double> overhead_us;
  {
    PipelineBench pb(packets, false);
    (void)pb.RunInlineOverheadNs();  // warm-up
    for (int i = 0; i < std::max(1, iterations / 4); ++i) overhead_us.push_back(pb.RunInlineOverheadNs() / 1000.0);
  }

  const double v_p50 = Percentile(validate_ms, 0.5), v_p90 = Percentile(validate_ms, 0.9);
  const double l_p50 = Percentile(latency_us, 0.5), l_p99 = Percentile(latency_us, 0.99);
  const double o_p50 = Percentile(overhead_us, 0.5);

  bool ok = true;
  if (!GE_BENCH_SANITIZED && GE_BENCH_OPTIMIZED) {
    ok = v_p50 < 50.0 && l_p50 < 50.0 && o_p50 < 2.0;
  }
  if (json) {
    std::printf(
        "{\"validate_100n_300e_ms\":{\"p50\":%.3f,\"p90\":%.3f,\"budget\":50},"
        "\"pipeline_latency_us\":{\"p50\":%.3f,\"p99\":%.3f,\"budget_p50\":50,\"nodes\":%d,\"threads\":%u,\"packets\":%lld},"
        "\"schedule_overhead_us\":{\"p50\":%.4f,\"budget\":2},"
        "\"sanitized\":%s,\"optimized\":%s,\"ok\":%s}\n",
        v_p50, v_p90, l_p50, l_p99, kPipelineNodes, threads, static_cast<long long>(latency_packets), o_p50,
        GE_BENCH_SANITIZED ? "true" : "false", GE_BENCH_OPTIMIZED ? "true" : "false", ok ? "true" : "false");
  } else {
    std::printf("validate 100N/300E        p50 %.3f ms   p90 %.3f ms   (budget 50 ms)\n", v_p50, v_p90);
    std::printf("pipeline latency 10N/%uT   p50 %.3f us   p99 %.3f us   (budget p50 50 us, %lld packets)\n", threads,
                l_p50, l_p99, static_cast<long long>(latency_packets));
    std::printf("schedule overhead/node    p50 %.4f us                (budget 2 us)\n", o_p50);
    std::printf("%s\n", !GE_BENCH_OPTIMIZED ? "UNOPTIMIZED BUILD - budgets not applied"
                                           : (ok ? "OK" : "BUDGET EXCEEDED"));
  }
  return ok ? 0 : 1;
}
