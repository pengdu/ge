#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <ge/cpp/engine.h>
#include <ge/media/operators.h>
#include <ge/media/rendition.h>
#include <ge/media/testing.h>
#include <ge/media/transcode_controller.h>

// P6 acceptance (15 §9 / 03): the media operators run through the Engine
// as builtins. Every test generates its own input with libavfilter and
// checks the outputs by re-reading them with libavformat (plus ffprobe
// when present).

namespace {

namespace fs = std::filesystem;
using namespace ge::media;

fs::path TempDir(const char* name) {
  const fs::path dir = fs::temp_directory_path() / ("ge_media_" + std::string(name) + "_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

struct Fixture {
  fs::path dir;
  std::string input;
  SampleInfo sample;
  std::unique_ptr<ge::Engine> engine;

  explicit Fixture(const char* name, int frames = 90, int fps = 30, bool threads = false) : dir(TempDir(name)) {
    input = (dir / "in.mp4").string();
    SampleSpec spec;
    spec.path = input;
    spec.frames = frames;
    spec.fps = fps;
    spec.gop = fps;
    auto s = GenerateSample(spec);
    EXPECT_TRUE(s.ok()) << s.status().ToString();
    if (s.ok()) sample = *s;
    ge::EngineConfig c;
    c.cpu_threads = threads ? 4 : 0;
    c.watchdog_thread = threads;
    c.builtin_operators = MakeMediaOperatorFactory();
    auto e = ge::Engine::Create(std::move(c));
    EXPECT_TRUE(e.ok()) << e.status().ToString();
    engine = std::move(*e);
  }
  ~Fixture() {
    engine.reset();
    if (!::getenv("GE_KEEP")) fs::remove_all(dir);
  }

  RenditionTemplate::BaseOptions Base() const {
    RenditionTemplate::BaseOptions b;
    b.input_path = input;
    return b;
  }
  RenditionSpec Rendition(const char* id, int w, int h, const char* container = "flv") const {
    RenditionSpec r;
    r.id = id;
    r.width = w;
    r.height = h;
    r.bitrate_kbps = 800;
    r.gop = 30;
    r.container = container;
    r.output_path = (dir / (std::string(id) + "." + (std::string(container) == "matroska" ? "mkv" : container))).string();
    return r;
  }
  std::string Out(const char* id, const char* ext = "flv") const { return (dir / (std::string(id) + "." + ext)).string(); }

  // Runs the inline engine until the session stopped (or |max_turns|).
  bool RunToStop(ge::Session* s, int max_turns = 200000) {
    for (int i = 0; i < max_turns; ++i) {
      if (s->WaitStopped(std::chrono::milliseconds(engine->config().cpu_threads == 0 ? 0 : 5))) return true;
      if (engine->config().cpu_threads == 0) {
        (void)s->PumpMutations();
        engine->Tick();
      }
    }
    return false;
  }
  // Advances the inline engine by |turns| single executor tasks (Tick would
  // run everything that is queued, i.e. the whole file).
  void Turns(ge::Session* s, int turns) {
    for (int i = 0; i < turns; ++i) {
      (void)s->PumpMutations();
      if (!engine->executor().RunOne()) engine->Tick();
      s->Tick();
    }
  }
  std::int64_t PacketsOut(ge::Session* s, const std::string& node) const {
    const ge::JsonValue snap = s->Snapshot();
    for (const ge::JsonValue& n : snap.Find("nodes")->as_array()) {
      if (n.GetString("id") == node) return n.Find("metrics")->GetInteger("packets_out").value_or(0);
    }
    return -1;
  }
};

ProbeResult Probe(const std::string& path) {
  auto r = ProbeOutput(path);
  EXPECT_TRUE(r.ok()) << path << ": " << r.status().ToString();
  return r.ok() ? *r : ProbeResult{};
}

}  // namespace

// ---------------------------------------------------------------------------
// Capabilities / template
// ---------------------------------------------------------------------------

TEST(MediaCapabilityTest, OperatorsRegisterAndNegotiate) {
  auto f = MakeMediaOperatorFactory();
  for (std::string_view key : {kOpMediaDemux, kOpVideoDecode, kOpAudioDecode, kOpVideoScale, kOpVideoConvert, kOpVideoEncode,
                               kOpAudioEncode, kOpMediaMux}) {
    EXPECT_NE(f->Describe(*ge::OperatorKey::Parse(key)), nullptr) << key;
  }
  EXPECT_EQ(VideoEncodeCapability().parameters.hot_updatable, (std::vector<std::string>{"bitrate_kbps", "gop", "force_idr"}));
  EXPECT_EQ(VideoScaleCapability().parameters.hot_updatable, (std::vector<std::string>{"watermark"}));
  EXPECT_FALSE(PreferredVideoEncoder("h264").empty());
}

TEST(RenditionTemplateTest, GraphAndPatchesUseStableIds) {
  RenditionTemplate::BaseOptions base;
  base.input_path = "x.mp4";
  RenditionSpec r;
  r.id = "p720";
  r.output_path = "o.flv";
  auto g = RenditionTemplate::Graph(base, {r});
  ASSERT_TRUE(g.ok()) << g.status().ToString();
  EXPECT_EQ(g->nodes().size(), 7U);  // demux vdec adec aenc + scale venc mux
  EXPECT_EQ(g->edges().size(), 7U);
  EXPECT_NE(g->FindEdge("r.p720.e.audio"), nullptr);

  auto add = RenditionTemplate::AddRendition(r, base);
  ASSERT_TRUE(add.ok());
  EXPECT_EQ(add->actions.size(), 7U);
  const ge::MutationPatch rm = RenditionTemplate::RemoveRendition("p720", ge::RemovePolicy::kFast, true, base);
  ASSERT_EQ(rm.actions.size(), 1U);
  EXPECT_EQ(rm.remove_policy, ge::RemovePolicy::kFast);
  const auto& rb = std::get<ge::RemoveBranchAction>(rm.actions[0]);
  EXPECT_EQ(rb.entry_edges, (std::vector<std::string>{"r.p720.e.video", "r.p720.e.audio"}));

  r.width = 719;
  EXPECT_EQ(RenditionTemplate::ValidateSpec(r).code(), GE_STATUS_INVALID_ARGUMENT);
  r.width = 1280;
  r.id = "bad id";
  EXPECT_EQ(RenditionTemplate::ValidateSpec(r).code(), GE_STATUS_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// End-to-end transcode
// ---------------------------------------------------------------------------

TEST(MediaTranscodeTest, SampleIsProbeable) {
  Fixture f("sample", 60);
  const ProbeResult p = Probe(f.input);
  EXPECT_EQ(p.video_packets, 60);
  EXPECT_GT(p.audio_packets, 0);
  EXPECT_TRUE(p.first_video_is_key);
  EXPECT_TRUE(p.read_to_eof);
  EXPECT_TRUE(p.trailer_ok);
}

TEST(MediaTranscodeTest, FlvAndMp4RenditionsPlayableAfterDrain) {
  Fixture f("basic", 60);
  const std::vector<RenditionSpec> rs{f.Rendition("r360", 640, 360, "flv"), f.Rendition("r180", 320, 180, "mp4")};
  auto g = RenditionTemplate::Graph(f.Base(), rs);
  ASSERT_TRUE(g.ok()) << g.status().ToString();
  auto s = f.engine->CreateSession(*g);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  ASSERT_TRUE(session->Start().ok());
  ASSERT_TRUE(f.RunToStop(session)) << session->Snapshot().Serialize();
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();
  // Zero loss at every stage (packets_out counts per consumer edge).
  EXPECT_EQ(f.PacketsOut(session, "vdec"), 60 * static_cast<std::int64_t>(rs.size()));
  for (const RenditionSpec& r : rs) {
    EXPECT_EQ(f.PacketsOut(session, "r." + r.id + ".scale"), 60) << r.id;
    EXPECT_EQ(f.PacketsOut(session, "r." + r.id + ".venc"), 60) << r.id;
  }

  for (const RenditionSpec& r : rs) {
    const ProbeResult p = Probe(r.output_path);
    EXPECT_EQ(p.width, r.width) << r.id;
    EXPECT_EQ(p.height, r.height) << r.id;
    EXPECT_EQ(p.video_packets, 60) << r.id << " " << p.ToJson().Serialize();
    EXPECT_GT(p.audio_packets, 0) << r.id;
    EXPECT_TRUE(p.first_video_is_key) << r.id;
    EXPECT_LE(p.av_offset_ms, 40) << r.id;
    EXPECT_TRUE(p.read_to_eof) << r.id;
    EXPECT_TRUE(p.trailer_ok) << r.id << " " << p.container;
    EXPECT_EQ(p.audio_codec, "aac");
    // Monotonic video timestamps, every GOP starts with a keyframe.
    for (std::size_t i = 1; i < p.video_pts_ms.size(); ++i) EXPECT_GT(p.video_pts_ms[i], p.video_pts_ms[i - 1]) << r.id;
    EXPECT_GE(p.video_keyframes, 2) << r.id;
    if (const auto n = FfprobeVideoFrames(r.output_path)) {
      EXPECT_EQ(*n, 60) << r.id;
    }
  }
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}

TEST(MediaTranscodeTest, AddRenditionMidStreamStartsWithIdrAndLeavesOthersIntact) {
  Fixture f("add", 90);
  const RenditionSpec r0 = f.Rendition("base", 640, 360, "flv");
  auto g = RenditionTemplate::Graph(f.Base(), {r0});
  ASSERT_TRUE(g.ok());
  auto s = f.engine->CreateSession(*g);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  TranscodeController ctl(*f.engine, *session, f.Base(), {r0});
  ASSERT_TRUE(session->Start().ok());

  // Let ~30 frames through before adding.
  while (f.PacketsOut(session, "r.base.venc") < 30) f.Turns(session, 1);
  const RenditionSpec r1 = f.Rendition("late", 320, 180, "mp4");
  auto op = ctl.AddRendition(r1);
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  auto rec = ctl.Wait(*op, std::chrono::seconds(10));
  ASSERT_TRUE(rec.ok()) << rec.status().ToString();
  EXPECT_EQ(rec->state, ge::OperationState::kSucceeded) << rec->result.ToString();
  EXPECT_EQ(ctl.Renditions().size(), 2U);

  ASSERT_TRUE(f.RunToStop(session));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();

  const ProbeResult base = Probe(r0.output_path);
  EXPECT_EQ(base.video_packets, 90) << base.ToJson().Serialize();  // TR-A-5: zero loss on the existing rendition
  EXPECT_TRUE(base.trailer_ok);
  const ProbeResult late = Probe(r1.output_path);
  EXPECT_TRUE(late.first_video_is_key);  // TR-A-3
  EXPECT_GT(late.video_packets, 0);
  EXPECT_LT(late.video_packets, 90);  // started mid-stream (TR-A-2)
  EXPECT_LE(late.av_offset_ms, 40) << late.ToJson().Serialize();  // TR-A-4
  EXPECT_TRUE(late.read_to_eof);
  EXPECT_TRUE(late.trailer_ok);
  EXPECT_EQ(ctl.stats().node_failures, 0U);
  EXPECT_GE(ctl.stats().format_changes, 2U);  // one per encoder
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}

TEST(MediaTranscodeTest, DrainRemovalFinishesFileFastRemovalDoesNotBlockOthers) {
  Fixture f("remove", 90);
  const RenditionSpec keep = f.Rendition("keep", 640, 360, "flv");
  const RenditionSpec drain = f.Rendition("drain", 320, 180, "mp4");
  const RenditionSpec fast = f.Rendition("fast", 320, 180, "flv");
  auto g = RenditionTemplate::Graph(f.Base(), {keep, drain, fast});
  ASSERT_TRUE(g.ok());
  auto s = f.engine->CreateSession(*g);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  TranscodeController ctl(*f.engine, *session, f.Base(), {keep, drain, fast});
  ASSERT_TRUE(session->Start().ok());
  while (f.PacketsOut(session, "r.keep.venc") < 30) f.Turns(session, 1);

  auto op1 = ctl.RemoveRendition("drain", true);
  ASSERT_TRUE(op1.ok()) << op1.status().ToString();
  auto op2 = ctl.RemoveRendition("fast", false);
  ASSERT_TRUE(op2.ok()) << op2.status().ToString();
  for (const ge::OperationId op : {*op1, *op2}) {
    auto rec = ctl.Wait(op, std::chrono::seconds(10));
    ASSERT_TRUE(rec.ok()) << rec.status().ToString();
    EXPECT_EQ(rec->state, ge::OperationState::kSucceeded) << rec->result.ToString();
  }
  EXPECT_EQ(ctl.Renditions().size(), 1U);
  EXPECT_EQ(ctl.stats().drain_timeouts, 0U);

  // Resources are reusable right away: add a rendition with the drained id.
  RenditionSpec again = drain;
  again.output_path = f.Out("again", "mp4");
  auto op3 = ctl.AddRendition(again);
  ASSERT_TRUE(op3.ok()) << op3.status().ToString();
  auto rec3 = ctl.Wait(*op3, std::chrono::seconds(10));
  ASSERT_TRUE(rec3.ok());
  EXPECT_EQ(rec3->state, ge::OperationState::kSucceeded) << rec3->result.ToString();

  ASSERT_TRUE(f.RunToStop(session));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();

  const ProbeResult k = Probe(keep.output_path);
  EXPECT_EQ(k.video_packets, 90);  // TR-C-1
  EXPECT_TRUE(k.trailer_ok);
  const ProbeResult d = Probe(drain.output_path);  // TR-D-6: complete MP4 after drain
  EXPECT_GT(d.video_packets, 0);
  EXPECT_LT(d.video_packets, 90);
  EXPECT_TRUE(d.read_to_eof);
  EXPECT_TRUE(d.trailer_ok) << d.ToJson().Serialize();
  const ProbeResult a = Probe(again.output_path);
  EXPECT_GT(a.video_packets, 0);
  EXPECT_TRUE(a.first_video_is_key);
  EXPECT_TRUE(a.trailer_ok);
  // Fast removal: the file exists; the mux's Close still tries to finish it.
  EXPECT_TRUE(fs::exists(fast.output_path));
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}

TEST(MediaTranscodeTest, HotUpdatesBitrateGopWatermarkAndForcedIdr) {
  Fixture f("hot", 120);
  RenditionSpec r = f.Rendition("r", 320, 180, "flv");
  r.gop = 60;
  auto g = RenditionTemplate::Graph(f.Base(), {r});
  ASSERT_TRUE(g.ok());
  auto s = f.engine->CreateSession(*g);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  TranscodeController ctl(*f.engine, *session, f.Base(), {r});
  ASSERT_TRUE(session->Start().ok());

  while (f.PacketsOut(session, "r.r.venc") < 20) f.Turns(session, 1);
  const std::int64_t before_idr = f.PacketsOut(session, "r.r.venc");
  auto k = ctl.RequestKeyFrame("r");
  ASSERT_TRUE(k.ok()) << k.status().ToString();

  while (f.PacketsOut(session, "r.r.venc") < 40) f.Turns(session, 1);
  const std::int64_t before_gop = f.PacketsOut(session, "r.r.venc");
  RenditionTemplate::HotUpdate u;
  u.bitrate_kbps = 300;
  u.gop = 10;
  WatermarkSpec wm;
  wm.x = 0;
  wm.y = 0;
  wm.w = 80;
  wm.h = 40;
  wm.color = "white";
  u.watermark = wm;
  auto p = ctl.UpdateRendition("r", u);
  ASSERT_TRUE(p.ok()) << p.status().ToString();
  EXPECT_EQ(ctl.Find("r")->gop, 10);
  EXPECT_EQ(ctl.Find("r")->bitrate_kbps, 300);
  EXPECT_TRUE(ctl.Find("r")->watermark.has_value());

  ASSERT_TRUE(f.RunToStop(session));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();

  const ProbeResult pr = Probe(r.output_path);
  EXPECT_EQ(pr.video_packets, 120) << pr.ToJson().Serialize();
  EXPECT_TRUE(pr.trailer_ok);
  // Forced IDR landed within a couple of frames of the request.
  bool idr_hit = false;
  for (const KeyFrameInfo& kf : pr.keyframes) {
    if (kf.index >= before_idr && kf.index <= before_idr + 3) idr_hit = true;
  }
  EXPECT_TRUE(idr_hit) << "keyframes: " << pr.ToJson().Serialize();
  // GOP 60 -> 10: after the update keyframes come every 10 frames (the
  // reopen itself starts with one).
  std::int64_t keys_after = 0;
  for (const KeyFrameInfo& kf : pr.keyframes) {
    if (kf.index >= before_gop) ++keys_after;
  }
  EXPECT_GE(keys_after, (120 - before_gop) / 10 - 1) << pr.ToJson().Serialize();
  // Watermark: the top-left box is bright at the end, not at the start.
  const double early = AverageLuma(r.output_path, 5, 0, 0, 80, 40);
  const double late = AverageLuma(r.output_path, 115, 0, 0, 80, 40);
  EXPECT_GE(early, 0);
  EXPECT_GT(late, 200) << "late=" << late << " early=" << early;
  EXPECT_LT(early, 200) << "late=" << late << " early=" << early;
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}

TEST(MediaTranscodeTest, SwitchCodecReplacesBranchWithoutStoppingSharedDecoder) {
  Fixture f("codec", 90);
  const RenditionSpec r0 = f.Rendition("h264", 320, 180, "mp4");
  auto g = RenditionTemplate::Graph(f.Base(), {r0});
  ASSERT_TRUE(g.ok());
  auto s = f.engine->CreateSession(*g);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  TranscodeController ctl(*f.engine, *session, f.Base(), {r0});
  ASSERT_TRUE(session->Start().ok());
  while (f.PacketsOut(session, "r.h264.venc") < 30) f.Turns(session, 1);

  RenditionSpec r1 = f.Rendition("mpeg4", 320, 180, "mp4");
  r1.codec = "mpeg4";
  const auto t0 = std::chrono::steady_clock::now();
  auto op = ctl.SwitchCodec("h264", r1);
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  auto rec = ctl.Wait(*op, std::chrono::seconds(10));
  ASSERT_TRUE(rec.ok()) << rec.status().ToString();
  EXPECT_EQ(rec->state, ge::OperationState::kSucceeded) << rec->result.ToString();
  // TR-U-3: the new branch is up (its first packet is an IDR) well within 500ms.
  while (f.PacketsOut(session, "r.mpeg4.venc") < 1) f.Turns(session, 1);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(500));
  EXPECT_EQ(ctl.Find("h264"), std::nullopt);
  ASSERT_TRUE(ctl.Find("mpeg4").has_value());

  ASSERT_TRUE(f.RunToStop(session));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();
  const ProbeResult a = Probe(r0.output_path);
  const ProbeResult b = Probe(r1.output_path);
  EXPECT_EQ(a.video_codec, "h264");
  EXPECT_EQ(b.video_codec, "mpeg4");
  EXPECT_TRUE(a.trailer_ok);
  EXPECT_TRUE(b.trailer_ok);
  EXPECT_TRUE(b.first_video_is_key);
  EXPECT_LE(b.av_offset_ms, 40) << b.ToJson().Serialize();
  // No frame of the source was lost across the switch.
  EXPECT_GE(a.video_packets + b.video_packets, 90) << a.video_packets << "+" << b.video_packets;
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}

TEST(MediaTranscodeTest, ThreadedEngineRealtimeAddAndRemove) {
  Fixture f("threads", 60, 30, /*threads=*/true);
  RenditionTemplate::BaseOptions base = f.Base();
  base.realtime = true;
  const RenditionSpec r0 = f.Rendition("a", 320, 180, "flv");
  auto g = RenditionTemplate::Graph(base, {r0});
  ASSERT_TRUE(g.ok());
  auto s = f.engine->CreateSession(*g);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  TranscodeController ctl(*f.engine, *session, base, {r0});
  ASSERT_TRUE(session->Start().ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const RenditionSpec r1 = f.Rendition("b", 320, 180, "mp4");
  auto add = ctl.AddRendition(r1);
  ASSERT_TRUE(add.ok()) << add.status().ToString();
  auto rec = ctl.Wait(*add, std::chrono::seconds(5));
  ASSERT_TRUE(rec.ok()) << rec.status().ToString();
  EXPECT_EQ(rec->state, ge::OperationState::kSucceeded) << rec->result.ToString();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  auto rm = ctl.RemoveRendition("b", true);
  ASSERT_TRUE(rm.ok()) << rm.status().ToString();
  auto rrec = ctl.Wait(*rm, std::chrono::seconds(5));
  ASSERT_TRUE(rrec.ok()) << rrec.status().ToString();
  EXPECT_EQ(rrec->state, ge::OperationState::kSucceeded) << rrec->result.ToString();
  ASSERT_TRUE(session->WaitStopped(std::chrono::seconds(10)));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();
  const ProbeResult a = Probe(r0.output_path);
  EXPECT_EQ(a.video_packets, 60);
  EXPECT_TRUE(a.trailer_ok);
  const ProbeResult b = Probe(r1.output_path);
  EXPECT_GT(b.video_packets, 0);
  EXPECT_TRUE(b.first_video_is_key);
  EXPECT_TRUE(b.trailer_ok) << b.ToJson().Serialize();
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}
