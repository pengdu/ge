// 7x24 soak (15 §11 task 6, 00 REL/OBS acceptance): the two target scenes
// running side by side on one threaded Engine for a configurable wall time.
//
//   ge_soak --duration 1h [--report 60s] [--input in.mp4] [--model x.onnx]
//           [--threads 4] [--out DIR] [--json]
//
// Scene A (transcode): a looping realtime demux feeding a rendition ladder.
// Every ~7s a rendition is added or drain-removed, every ~3s a hot update
// (bitrate/gop/watermark/IDR) lands, every ~40s one rendition switches
// codec/container. Outputs go to DIR and are truncated by re-adding.
//
// Scene B (inference): a synthetic tensor source through OnnxInfer to a
// checking sink, session recreated every ~30s (create/start/drain/destroy
// cycle) with the batch size flipped between runs; skipped when the model or
// ONNX Runtime is missing.
//
// Watchers: every --report the program prints one line with RSS, executor /
// async in-flight, ledger reservations, end-to-end P99 and audit/mutation
// counters, and remembers the RSS series. The exit code is non-zero when a
// session failed, a mutation failed for a reason other than resource
// exhaustion, an inference result was wrong, the ledger did not return to
// its idle level after the sessions were gone, or RSS grew monotonically
// over the last half of the run beyond --rss-slack MB (leak heuristic).
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ge/cpp/engine.h>
#include <ge/cpp/graph_spec.h>
#include <ge/cpp/session.h>
#include <ge/media/operators.h>
#include <ge/media/rendition.h>
#include <ge/media/testing.h>
#include <ge/media/transcode_controller.h>
#include <ge/infer/onnx.h>
#include <ge/infer/tensor.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

std::chrono::seconds ParseDuration(const std::string& s) {
  char unit = 's';
  std::string num = s;
  if (!s.empty() && std::isalpha(static_cast<unsigned char>(s.back()))) {
    unit = s.back();
    num = s.substr(0, s.size() - 1);
  }
  const long v = std::stol(num);
  switch (unit) {
    case 'h': return std::chrono::seconds(v * 3600);
    case 'm': return std::chrono::seconds(v * 60);
    default: return std::chrono::seconds(v);
  }
}

struct Options {
  std::chrono::seconds duration{3600};
  std::chrono::seconds report{60};
  std::string input;
  std::string model = GE_SOAK_MODEL;
  std::uint32_t threads = 4;
  fs::path out = fs::temp_directory_path() / "ge_soak";
  bool json = false;
  double rss_slack_mb = 64;
  bool no_transcode = false;
  bool no_infer = false;
};

// ---------------------------------------------------------------------------
// Process watchers
// ---------------------------------------------------------------------------

double RssMb() {
#if defined(__APPLE__)
  mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
  }
  return -1;
#elif defined(__linux__)
  std::ifstream f("/proc/self/statm");
  long size = 0, resident = 0;
  f >> size >> resident;
  return static_cast<double>(resident) * static_cast<double>(::sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#else
  return -1;
#endif
}

long OpenFds() {
#if defined(__APPLE__) || defined(__linux__)
  long n = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator("/dev/fd", ec)) {
    (void)e;
    ++n;
  }
  return ec ? -1 : n;
#else
  return -1;
#endif
}

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }

struct Verdict {
  std::mutex mutex;
  std::vector<std::string> failures;
  void Fail(std::string what) {
    std::lock_guard lock(mutex);
    std::fprintf(stderr, "[soak] FAIL: %s\n", what.c_str());
    failures.push_back(std::move(what));
  }
  bool ok() {
    std::lock_guard lock(mutex);
    return failures.empty();
  }
};

// ---------------------------------------------------------------------------
// Scene A: transcode ladder with continuous mutations
// ---------------------------------------------------------------------------

struct TranscodeScene {
  std::atomic<std::uint64_t> adds{0}, removes{0}, hot_updates{0}, switches{0}, resource_rejections{0};
  std::atomic<std::uint64_t> failed_ops{0};

  void Run(ge::Engine& engine, const Options& opt, Verdict& verdict) {
    using namespace ge::media;
    RenditionTemplate::BaseOptions base;
    base.input_path = opt.input;
    base.realtime = true;
    base.loop = true;
    auto rend = [&](const char* id, int w, int h, const char* container) {
      RenditionSpec r;
      r.id = id;
      r.width = w;
      r.height = h;
      r.bitrate_kbps = w >= 640 ? 1200 : 500;
      r.gop = 30;
      r.container = container;
      r.output_path = (opt.out / (std::string(id) + "." + (std::string(container) == "matroska" ? "mkv" : container))).string();
      return r;
    };
    const RenditionSpec r360 = rend("r360", 640, 360, "flv");
    auto g = RenditionTemplate::Graph(base, {r360});
    if (!g.ok()) return verdict.Fail("transcode graph: " + g.status().ToString());
    auto s = engine.CreateSession(*g, ge::CallerContext{"soak", "transcode"});
    if (!s.ok()) return verdict.Fail("transcode create: " + s.status().ToString());
    ge::Session* session = *s;
    TranscodeController ctl(engine, *session, base, {r360});
    if (ge::Status st = session->Start(); !st.ok()) return verdict.Fail("transcode start: " + st.ToString());

    const RenditionSpec ladder[] = {rend("r180", 320, 180, "mp4"), rend("r240", 426, 240, "flv"),
                                    rend("r270", 480, 270, "matroska")};
    bool present[3] = {false, false, false};
    std::map<std::string, Clock::time_point> added_at;  // rendition id -> add time
    added_at[r360.id] = Clock::now();
    auto wait = [&](ge::Result<ge::OperationId> op, const char* what) {
      if (!op.ok()) {
        if (op.status().code() == GE_STATUS_RESOURCE_EXHAUSTED) {
          ++resource_rejections;
          return false;
        }
        ++failed_ops;
        verdict.Fail(std::string(what) + ": " + op.status().ToString());
        return false;
      }
      auto rec = ctl.Wait(*op, std::chrono::seconds(30));
      if (!rec.ok()) {
        ++failed_ops;
        verdict.Fail(std::string(what) + " wait: " + rec.status().ToString());
        return false;
      }
      if (rec->state != ge::OperationState::kSucceeded) {
        if (rec->result.code() == GE_STATUS_RESOURCE_EXHAUSTED) {
          ++resource_rejections;
          return false;
        }
        ++failed_ops;
        verdict.Fail(std::string(what) + ": " + rec->result.ToString());
        return false;
      }
      return true;
    };

    const auto t0 = Clock::now();
    unsigned step = 0;
    int codec_switch_gen = 0;
    std::string main_id = "r360";  // flips to r360b on every codec switch
    while (!g_stop.load()) {
      if (session->state() == ge::SessionState::kFailed) {
        verdict.Fail("transcode session failed: " + session->failure().ToString());
        break;
      }
      if (session->state() == ge::SessionState::kStopped) {
        verdict.Fail("transcode session stopped unexpectedly");
        break;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
      ++step;
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t0).count();
      if (step % 3 == 0) {
        RenditionTemplate::HotUpdate u;
        switch ((step / 3) % 4) {
          case 0: u.bitrate_kbps = 300 + 100 * static_cast<std::int64_t>(step % 7); break;
          case 1: u.gop = 15 + static_cast<int>(step % 30); break;
          case 2: {
            WatermarkSpec wm;
            wm.x = 8;
            wm.y = 8;
            wm.w = 64;
            wm.h = 32;
            u.watermark = wm;
            break;
          }
          default: u.force_idr = true; break;
        }
        auto r = ctl.UpdateRendition(main_id, u);
        if (!r.ok()) {
          ++failed_ops;
          verdict.Fail("hot update: " + r.status().ToString());
        } else {
          ++hot_updates;
        }
        if ((step / 3) % 4 == 2) {
          RenditionTemplate::HotUpdate clear;
          clear.clear_watermark = true;
          (void)ctl.UpdateRendition(main_id, clear);
        }
      }
      if (step % 7 == 0) {
        const std::size_t i = (step / 7) % 3;
        if (!present[i]) {
          std::error_code ec;
          fs::remove(ladder[i].output_path, ec);
          if (wait(ctl.AddRendition(ladder[i]), "add rendition")) {
            present[i] = true;
            added_at[ladder[i].id] = Clock::now();
            ++adds;
          }
        } else {
          if (wait(ctl.RemoveRendition(ladder[i].id, /*drain=*/(step / 7) % 2 == 0), "remove rendition")) {
            present[i] = false;
            ++removes;
          }
        }
      }
      if (elapsed > 0 && step % 40 == 0) {
        // Switch the always-on rendition's container: r360 <-> r360b.
        const bool to_b = codec_switch_gen % 2 == 0;
        RenditionSpec replacement = rend(to_b ? "r360b" : "r360", 640, 360, to_b ? "mp4" : "flv");
        std::error_code ec;
        fs::remove(replacement.output_path, ec);
        if (wait(ctl.SwitchCodec(main_id, replacement), "switch codec")) {
          main_id = replacement.id;
          added_at[main_id] = Clock::now();
          ++switches;
          ++codec_switch_gen;
        }
      }
    }
    // Graceful drain so the files get their trailers, then verify them. A
    // branch added moments before the stop may legitimately hold no video
    // yet (the shared decoder's next frame had not arrived), so playability
    // is only demanded of renditions that lived >= 2s.
    const Clock::time_point stop_at = Clock::now();
    auto stop = session->Stop(false, ge::CallerContext{"soak", "transcode-stop"});
    if (stop.ok()) (void)ctl.Wait(*stop, std::chrono::seconds(30));
    if (!session->WaitStopped(std::chrono::seconds(30))) {
      (void)session->Stop(true);
      (void)session->WaitStopped(std::chrono::seconds(30));
      verdict.Fail("transcode graceful stop timed out");
    }
    for (const RenditionSpec& r : ctl.Renditions()) {
      auto p = ProbeOutput(r.output_path);
      if (!p.ok()) {
        verdict.Fail("probe " + r.output_path + ": " + p.status().ToString());
        continue;
      }
      const auto it = added_at.find(r.id);
      const bool mature = it == added_at.end() || stop_at - it->second >= std::chrono::seconds(2);
      if (!p->trailer_ok || (mature && (p->video_packets <= 0 || !p->first_video_is_key))) {
        verdict.Fail("output " + r.output_path + " unplayable: " + p->ToJson().Serialize());
      }
    }
    const TranscodeStats st = ctl.stats();
    if (st.node_failures != 0) verdict.Fail("transcode node failures: " + std::to_string(st.node_failures));
    if (ge::Status d = engine.DestroySession(session->id()); !d.ok()) verdict.Fail("transcode destroy: " + d.ToString());
  }
};

// ---------------------------------------------------------------------------
// Scene B: inference cycles
// ---------------------------------------------------------------------------

constexpr std::int64_t kC = 3, kH = 224, kW = 224;
constexpr std::size_t kSample = static_cast<std::size_t>(kC * kH * kW);

// Deterministic "image" i (same family as ge_onnx_infer_test).
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

// Emits |count| tensors; recycles 16 distinct samples so the sink can check
// argmax stability without a reference model run.
class SoakTensorSource final : public ge::Operator {
 public:
  SoakTensorSource(std::int64_t count, std::shared_ptr<ge::HostBufferPool> pool) : count_(count), pool_(std::move(pool)) {
    for (int i = 0; i < kDistinct; ++i) {
      samples_.emplace_back(kSample);
      FillSample(i, samples_.back().data());
    }
  }
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    if (next_ >= count_) return ge::ProcessResult::kExhausted;
    ge::BufferRef buf = pool_->Allocate(kSample * sizeof(float));
    std::memcpy(buf->data, samples_[static_cast<std::size_t>(next_ % kDistinct)].data(), kSample * sizeof(float));
    buf->size = kSample * sizeof(float);
    ge::infer::TensorFormat f;
    f.dtype = ge::infer::DType::kFloat32;
    f.shape = {kC, kH, kW};
    ++next_;
    ge::Packet p = ge::infer::MakeTensorPacket(std::move(buf), f, static_cast<ge::PacketSeq>(next_), next_ * 33'000'000);
    ge::Status s = req.sink->Emit("out", std::move(p));
    if (s.code() == GE_STATUS_WOULD_BLOCK) {
      --next_;
    } else if (!s.ok()) {
      return s;
    }
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }
  static constexpr int kDistinct = 16;

 private:
  std::int64_t count_;
  std::shared_ptr<ge::HostBufferPool> pool_;
  std::int64_t next_ = 0;
  std::vector<std::vector<float>> samples_;
};

// Checks: argmax of sample k is the same every time k comes around (the
// first run establishes it), seqs are consumed in order.
struct InferCheck {
  std::mutex mutex;
  std::array<int, SoakTensorSource::kDistinct> expected{};
  std::atomic<std::uint64_t> results{0}, mismatches{0}, out_of_order{0};
  InferCheck() { expected.fill(-1); }
};

class SoakLogitsSink final : public ge::Operator {
 public:
  explicit SoakLogitsSink(InferCheck& check) : check_(check) {}
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    for (const ge::PacketRef& p : req.inputs) {
      if (p->is_event() || p->is_eos()) continue;
      auto f = ge::infer::FormatOf(*p);
      if (!f.ok()) return f.status();
      const float* d = static_cast<const float*>(p->payload->data);
      const std::size_t n = f->ElementCount();
      int arg = 0;
      for (std::size_t i = 1; i < n; ++i) {
        if (d[i] > d[arg]) arg = static_cast<int>(i);
      }
      const int k = static_cast<int>((p->header.seq - 1) % SoakTensorSource::kDistinct);
      {
        std::lock_guard lock(check_.mutex);
        if (check_.expected[static_cast<std::size_t>(k)] < 0) {
          check_.expected[static_cast<std::size_t>(k)] = arg;
        } else if (check_.expected[static_cast<std::size_t>(k)] != arg) {
          ++check_.mismatches;
        }
      }
      if (p->header.seq <= last_seq_) ++check_.out_of_order;
      last_seq_ = p->header.seq;
      ++check_.results;
    }
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }

 private:
  InferCheck& check_;
  ge::PacketSeq last_seq_ = 0;
};

struct InferScene {
  InferCheck check;
  std::atomic<std::uint64_t> cycles{0}, resource_rejections{0};
  std::shared_ptr<ge::HostBufferPool> pool = ge::HostBufferPool::Create();

  void Register(ge::BuiltinOperatorFactory& factory) {
    ge::PortCapability out;
    out.name = "out";
    out.direction = ge::PortDirection::kOutput;
    out.type_tag = "Tensor";
    out.cardinality = ge::PortCardinality::kMulti;
    ge::CapabilityDescriptor src;
    src.op = *ge::OperatorKey::Parse("SoakTensorSrc@1.0.0");
    src.outputs = {out};
    src.execution.stateful = true;
    src.execution.max_parallelism = 1;
    factory.Register(src, [this](const ge::OperatorCreateArgs& a) {
      return std::make_unique<SoakTensorSource>(a.options.GetInteger("count").value_or(1000), pool);
    });
    ge::PortCapability in;
    in.name = "in";
    in.direction = ge::PortDirection::kInput;
    in.type_tag = "Tensor";
    ge::CapabilityDescriptor sink;
    sink.op = *ge::OperatorKey::Parse("SoakLogitsSink@1.0.0");
    sink.inputs = {in};
    sink.execution.stateful = true;
    sink.execution.max_parallelism = 1;
    factory.Register(sink, [this](const ge::OperatorCreateArgs&) { return std::make_unique<SoakLogitsSink>(check); });
  }

  void Run(ge::Engine& engine, const Options& opt, Verdict& verdict) {
    std::int64_t cycle = 0;
    while (!g_stop.load()) {
      const std::int64_t count = 600 + 200 * (cycle % 3);
      ge::JsonObject infer;
      infer["model"] = ge::JsonValue(opt.model);
      infer["intra_threads"] = ge::JsonValue(1);
      infer["workers"] = ge::JsonValue(1 + static_cast<int>(cycle % 2));
      ge::GraphBuilder b("soak-infer");
      auto s = b.AddNode(*ge::OperatorKey::Parse("SoakTensorSrc@1.0.0"), "src",
                         ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(count)}}));
      auto n = b.AddNode(*ge::OperatorKey::Parse("OnnxInfer@1.0.0"), "net", ge::JsonValue(std::move(infer)));
      auto k = b.AddNode(*ge::OperatorKey::Parse("SoakLogitsSink@1.0.0"), "sink");
      b.Connect(s.port("out"), n.port("in"), {.id = "e0"});
      b.Connect(n.port("out"), k.port("in"), {.id = "e1"});
      auto spec = b.Build();
      if (!spec.ok()) return verdict.Fail("infer graph: " + spec.status().ToString());
      auto sess = engine.CreateSession(*spec, ge::CallerContext{"soak", "infer-" + std::to_string(cycle)});
      if (!sess.ok()) {
        if (sess.status().code() == GE_STATUS_RESOURCE_EXHAUSTED) {
          ++resource_rejections;
          std::this_thread::sleep_for(std::chrono::seconds(2));
          continue;
        }
        return verdict.Fail("infer create: " + sess.status().ToString());
      }
      ge::Session* session = *sess;
      if (ge::Status st = session->Start(); !st.ok()) {
        verdict.Fail("infer start: " + st.ToString());
        (void)engine.DestroySession(session->id());
        return;
      }
      // Every other cycle is cut short with a fast stop while requests are
      // in flight (FastStop path); the rest drain to EOS.
      const bool fast = cycle % 4 == 3;
      bool stopped = false;
      const auto deadline = Clock::now() + std::chrono::seconds(fast ? 5 : 300);
      while (!stopped && Clock::now() < deadline && !g_stop.load()) {
        stopped = session->WaitStopped(std::chrono::milliseconds(200));
        if (session->state() == ge::SessionState::kFailed) break;
      }
      if (session->state() == ge::SessionState::kFailed) {
        verdict.Fail("infer session failed: " + session->failure().ToString());
      } else if (!stopped) {
        if (!fast && !g_stop.load()) verdict.Fail("infer cycle did not drain within 300s");
        (void)session->Stop(true);
        if (!session->WaitStopped(std::chrono::seconds(30))) verdict.Fail("infer fast stop timed out");
      }
      if (ge::Status d = engine.DestroySession(session->id()); !d.ok()) verdict.Fail("infer destroy: " + d.ToString());
      ++cycles;
      ++cycle;
    }
    if (check.mismatches.load() != 0) verdict.Fail("infer argmax mismatches: " + std::to_string(check.mismatches.load()));
    if (check.out_of_order.load() != 0) verdict.Fail("infer out-of-order results: " + std::to_string(check.out_of_order.load()));
  }
};

// ---------------------------------------------------------------------------
// Reporter
// ---------------------------------------------------------------------------

// First sample line of |series| (the HELP/TYPE lines start with '#').
std::string PromValue(const std::string& text, const std::string& series) {
  const auto p = text.find("\n" + series);
  if (p == std::string::npos) return "-";
  const auto sp = text.find(' ', p + 1);
  const auto nl = text.find('\n', sp);
  return text.substr(sp + 1, nl - sp - 1);
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--duration") opt.duration = ParseDuration(next());
    else if (a == "--report") opt.report = ParseDuration(next());
    else if (a == "--input") opt.input = next();
    else if (a == "--model") opt.model = next();
    else if (a == "--threads") opt.threads = static_cast<std::uint32_t>(std::stoul(next()));
    else if (a == "--out") opt.out = next();
    else if (a == "--rss-slack") opt.rss_slack_mb = std::stod(next());
    else if (a == "--json") opt.json = true;
    else if (a == "--no-transcode") opt.no_transcode = true;
    else if (a == "--no-infer") opt.no_infer = true;
    else {
      std::fprintf(stderr,
                   "usage: %s [--duration 1h] [--report 60s] [--input in.mp4] [--model x.onnx] [--threads T] "
                   "[--out DIR] [--rss-slack MB] [--no-transcode] [--no-infer] [--json]\n",
                   argv[0]);
      return 2;
    }
  }
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  ge::media::SetFfmpegLogLevel(ge::media::FfmpegLogLevel::kError);
  std::error_code ec;
  fs::create_directories(opt.out, ec);

  // Input: generate a 10 s sample when none is given (loops anyway).
  if (opt.input.empty() && !opt.no_transcode) {
    opt.input = (opt.out / "soak_in.mp4").string();
    ge::media::SampleSpec spec;
    spec.path = opt.input;
    spec.frames = 300;
    spec.fps = 30;
    spec.gop = 30;
    auto s = ge::media::GenerateSample(spec);
    if (!s.ok()) {
      std::fprintf(stderr, "[soak] cannot generate sample: %s\n", s.status().ToString().c_str());
      return 2;
    }
  }
  const bool infer_available = !opt.no_infer && ge::infer::OnnxRuntimeAvailable() && !opt.model.empty() &&
                               std::ifstream(opt.model).good();
  if (!opt.no_infer && !infer_available) {
    std::fprintf(stderr, "[soak] inference scene skipped (ONNX Runtime or model '%s' unavailable)\n", opt.model.c_str());
  }

  auto factory = ge::media::MakeMediaOperatorFactory();
  InferScene infer;
  if (infer_available) {
    ge::infer::RegisterOnnxInfer(*factory);
    infer.Register(*factory);
  }
  ge::EngineConfig cfg;
  cfg.cpu_threads = opt.threads;
  cfg.watchdog_thread = true;
  cfg.builtin_operators = factory;
  cfg.async.batching = true;
  cfg.async.max_batch = 4;
  cfg.async.batch_timeout = std::chrono::milliseconds(20);
  auto engine = ge::Engine::Create(std::move(cfg));
  if (!engine.ok()) {
    std::fprintf(stderr, "[soak] engine: %s\n", engine.status().ToString().c_str());
    return 2;
  }
  ge::Engine& e = **engine;
  Verdict verdict;
  TranscodeScene transcode;
  const std::size_t idle_leases = e.resource_ledger() != nullptr ? e.resource_ledger()->live_leases() : 0;

  std::vector<std::thread> threads;
  if (!opt.no_transcode) threads.emplace_back([&] { transcode.Run(e, opt, verdict); });
  if (infer_available) threads.emplace_back([&] { infer.Run(e, opt, verdict); });

  const auto t0 = Clock::now();
  const auto end = t0 + opt.duration;
  std::vector<double> rss_series;
  std::printf("[soak] start duration=%llds threads=%u out=%s transcode=%d infer=%d\n",
              static_cast<long long>(opt.duration.count()), opt.threads, opt.out.string().c_str(), !opt.no_transcode,
              infer_available);
  std::fflush(stdout);
  auto next_report = t0 + opt.report;
  while (Clock::now() < end && !g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (Clock::now() < next_report) continue;
    next_report += opt.report;
    const std::string prom = e.RenderPrometheus();
    const double rss = RssMb();
    rss_series.push_back(rss);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t0).count();
    std::printf(
        "[soak] t=%llds rss=%.1fMB fds=%ld sessions=%s async_inflight=%s leases=%s audit_dropped=%s "
        "e2e_p99_ms=%s adds=%llu removes=%llu hot=%llu switches=%llu res_rej=%llu infer_cycles=%llu "
        "infer_results=%llu mismatches=%llu\n",
        static_cast<long long>(elapsed), rss, OpenFds(), PromValue(prom, "ge_engine_sessions ").c_str(),
        PromValue(prom, "ge_engine_async_in_flight ").c_str(), PromValue(prom, "ge_engine_resource_leases ").c_str(),
        PromValue(prom, "ge_engine_audit_dropped_total ").c_str(),
        [&] {
          // P99 from the first session's end_to_end histogram is fine for a log line.
          for (const std::shared_ptr<ge::Session>& s : e.SessionRefs()) {
            const auto& h = s->scheduler().metrics().end_to_end;
            if (h.count() == 0) continue;
            const std::uint64_t p99 = h.P99();
            if (p99 == std::numeric_limits<std::uint64_t>::max()) return std::string(">17s");
            return std::to_string(static_cast<double>(p99) * 1e-6);
          }
          return std::string("-");
        }()
            .c_str(),
        static_cast<unsigned long long>(transcode.adds.load()), static_cast<unsigned long long>(transcode.removes.load()),
        static_cast<unsigned long long>(transcode.hot_updates.load()),
        static_cast<unsigned long long>(transcode.switches.load()),
        static_cast<unsigned long long>(transcode.resource_rejections.load() + infer.resource_rejections.load()),
        static_cast<unsigned long long>(infer.cycles.load()), static_cast<unsigned long long>(infer.check.results.load()),
        static_cast<unsigned long long>(infer.check.mismatches.load()));
    std::fflush(stdout);
    if (!verdict.ok()) break;
  }
  g_stop.store(true);
  for (std::thread& t : threads) t.join();

  // Post-run invariants.
  if (e.resource_ledger() != nullptr && e.resource_ledger()->live_leases() != idle_leases) {
    verdict.Fail("ledger leases did not return to idle: " + std::to_string(e.resource_ledger()->live_leases()) + " vs " +
                 std::to_string(idle_leases));
  }
  if (e.async_runtime().in_flight() != 0) verdict.Fail("async requests still in flight after shutdown");
  if (!e.Sessions().empty()) verdict.Fail("sessions left behind: " + std::to_string(e.Sessions().size()));
  // Leak heuristic (SLA-2 "no monotonic growth"): the mean RSS of the last
  // quarter of the run must not exceed the mean of the third quarter by more
  // than the slack. Quarter means are robust to allocator jitter; the first
  // half is ignored so pools / ORT arenas may warm up.
  double rss_q3 = 0, rss_q4 = 0;
  if (rss_series.size() >= 8) {
    const std::size_t q = rss_series.size() / 4;
    for (std::size_t i = 2 * q; i < 3 * q; ++i) rss_q3 += rss_series[i];
    for (std::size_t i = 3 * q; i < rss_series.size(); ++i) rss_q4 += rss_series[i];
    rss_q3 /= static_cast<double>(q);
    rss_q4 /= static_cast<double>(rss_series.size() - 3 * q);
    if (rss_q4 - rss_q3 > opt.rss_slack_mb) {
      verdict.Fail("RSS still growing: mean " + std::to_string(rss_q3) + " MB (3rd quarter) -> " +
                   std::to_string(rss_q4) + " MB (4th quarter), slack " + std::to_string(opt.rss_slack_mb));
    }
  }
  const std::string audit = e.audit().QueryJson(ge::AuditFilter{.failures_only = true}).Serialize();
  const std::size_t audit_failures = e.audit().Query(ge::AuditFilter{.failures_only = true}).size();

  const bool ok = verdict.ok();
  if (opt.json) {
    ge::JsonObject o;
    o["ok"] = ge::JsonValue(ok);
    o["duration_s"] = ge::JsonValue(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t0).count());
    o["rss_mb_first"] = ge::JsonValue(rss_series.empty() ? RssMb() : rss_series.front());
    o["rss_mb_last"] = ge::JsonValue(rss_series.empty() ? RssMb() : rss_series.back());
    o["rss_mb_q3_mean"] = ge::JsonValue(rss_q3);
    o["rss_mb_q4_mean"] = ge::JsonValue(rss_q4);
    o["transcode_adds"] = ge::JsonValue(transcode.adds.load());
    o["transcode_removes"] = ge::JsonValue(transcode.removes.load());
    o["transcode_hot_updates"] = ge::JsonValue(transcode.hot_updates.load());
    o["transcode_switches"] = ge::JsonValue(transcode.switches.load());
    o["resource_rejections"] = ge::JsonValue(transcode.resource_rejections.load() + infer.resource_rejections.load());
    o["infer_cycles"] = ge::JsonValue(infer.cycles.load());
    o["infer_results"] = ge::JsonValue(infer.check.results.load());
    o["audit_failures"] = ge::JsonValue(static_cast<std::uint64_t>(audit_failures));
    ge::JsonArray fails;
    for (const std::string& f : verdict.failures) fails.push_back(ge::JsonValue(f));
    o["failures"] = ge::JsonValue(std::move(fails));
    std::printf("%s\n", ge::JsonValue(std::move(o)).Serialize().c_str());
  } else {
    std::printf("[soak] %s: adds=%llu removes=%llu hot=%llu switches=%llu infer_cycles=%llu infer_results=%llu "
                "res_rej=%llu audit_failures=%zu\n",
                ok ? "PASS" : "FAIL", static_cast<unsigned long long>(transcode.adds.load()),
                static_cast<unsigned long long>(transcode.removes.load()),
                static_cast<unsigned long long>(transcode.hot_updates.load()),
                static_cast<unsigned long long>(transcode.switches.load()),
                static_cast<unsigned long long>(infer.cycles.load()),
                static_cast<unsigned long long>(infer.check.results.load()),
                static_cast<unsigned long long>(transcode.resource_rejections.load() + infer.resource_rejections.load()),
                audit_failures);
    if (!ok) std::printf("[soak] audit failures: %s\n", audit.c_str());
  }
  engine->reset();
  return ok ? 0 : 1;
}
