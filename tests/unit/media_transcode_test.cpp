#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ge/cpp/batch_runner.h>
#include <ge/cpp/engine.h>
#include <ge/cpp/graph_template.h>
#include <ge/cpp/mutation_applier.h>
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
  for (std::string_view key : {kOpMediaDemux, kOpVideoDecode, kOpAudioDecode, kOpVideoScale, kOpVideoConvert, kOpVideoFilter,
                               kOpVideoEncode, kOpAudioEncode, kOpMediaMux}) {
    EXPECT_NE(f->Describe(*ge::OperatorKey::Parse(key)), nullptr) << key;
  }
  EXPECT_EQ(VideoEncodeCapability().parameters.hot_updatable, (std::vector<std::string>{"bitrate_kbps", "gop", "force_idr"}));
  EXPECT_EQ(VideoScaleCapability().parameters.hot_updatable, (std::vector<std::string>{"watermark"}));
  EXPECT_EQ(VideoFilterCapability().parameters.hot_updatable, (std::vector<std::string>{"filter"}));
  EXPECT_TRUE(ValidateFilterChain("drawbox=x=0:y=0:w=8:h=8:color=white:t=fill").ok());
  EXPECT_EQ(ValidateFilterChain("nosuchfilter=1").code(), GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(ValidateFilterChain("drawbox=nosuchoption=1").code(), GE_STATUS_INVALID_ARGUMENT);
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

TEST(RenditionTemplateTest, FilterPatchesFollowTheEdgeThatFeedsTheEncoder) {
  RenditionTemplate::BaseOptions base;
  base.input_path = "x.mp4";
  RenditionSpec r;
  r.id = "p720";
  r.output_path = "o.flv";
  auto g = RenditionTemplate::Graph(base, {r});
  ASSERT_TRUE(g.ok());
  const RenditionNodeIds ids = RenditionTemplate::Ids("p720");
  EXPECT_EQ(ids.Filter("text"), "r.p720.f.text");

  // First filter splits the template's scale->venc edge.
  const FilterSpec text{"text", "drawbox=x=0:y=0:w=8:h=8:color=white:t=fill"};
  auto entry = RenditionTemplate::FilterEntryEdge(*g, "p720");
  ASSERT_TRUE(entry.ok());
  EXPECT_EQ(*entry, ids.scale_venc_edge);
  auto ins = RenditionTemplate::InsertFilter(*g, "p720", text);
  ASSERT_TRUE(ins.ok()) << ins.status().ToString();
  ASSERT_EQ(ins->actions.size(), 1U);
  const auto& ic = std::get<ge::InsertChainAction>(ins->actions[0]);
  EXPECT_EQ(ic.edge, ids.scale_venc_edge);
  ASSERT_EQ(ic.nodes.size(), 1U);
  EXPECT_EQ(ic.nodes[0].id, "r.p720.f.text");
  EXPECT_EQ(ic.nodes[0].op.ToString(), kOpVideoFilter);

  // Apply it to the spec the way the engine would; the next filter then
  // splits the derived edge between the first filter and the encoder.
  ge::MutationApplier applier(nullptr);
  auto cand = applier.Apply(*g, *ins);
  ASSERT_TRUE(cand.ok()) << cand.status().ToString();
  auto entry2 = RenditionTemplate::FilterEntryEdge(cand->candidate, "p720");
  ASSERT_TRUE(entry2.ok());
  EXPECT_EQ(*entry2, "r.p720.f.text.out->r.p720.venc.in");
  EXPECT_EQ(RenditionTemplate::InsertFilter(cand->candidate, "p720", text).status().code(), GE_STATUS_ALREADY_EXISTS);

  // Removal bypasses; the bypass edge is scale->venc again (derived name).
  const ge::MutationPatch rm = RenditionTemplate::RemoveFilter("p720", "text", ge::RemovePolicy::kDrain);
  ASSERT_EQ(rm.actions.size(), 1U);
  const auto& rc = std::get<ge::RemoveChainAction>(rm.actions[0]);
  EXPECT_EQ(rc.nodes, (std::vector<std::string>{"r.p720.f.text"}));
  EXPECT_EQ(rc.mode, ge::RemoveMode::kBypass);
  auto cand2 = applier.Apply(cand->candidate, rm);
  ASSERT_TRUE(cand2.ok()) << cand2.status().ToString();
  auto entry3 = RenditionTemplate::FilterEntryEdge(cand2->candidate, "p720");
  ASSERT_TRUE(entry3.ok());
  EXPECT_EQ(*entry3, "r.p720.scale.out->r.p720.venc.in");

  EXPECT_EQ(RenditionTemplate::ValidateFilter(FilterSpec{"bad id", "null"}).code(), GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(RenditionTemplate::ValidateFilter(FilterSpec{"f", ""}).code(), GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(RenditionTemplate::ValidateFilter(FilterSpec{"f", "nosuchfilter"}).code(), GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(RenditionTemplate::FilterEntryEdge(*g, "nope").status().code(), GE_STATUS_NOT_FOUND);
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

// 03 TR-U-6: a filter (here a white drawbox in the top-left corner, the
// same box the watermark test reads back) is inserted and removed on a
// live rendition several times. The rendition never stops (every frame of
// the input reaches the file, PTS stay monotonic), a sibling rendition is
// untouched, and the box is visible exactly while the filter is in.
TEST(MediaTranscodeTest, FilterInsertRemoveLoopKeepsRenditionContinuous) {
  Fixture f("filter", 150);
  const RenditionSpec r = f.Rendition("r", 320, 180, "flv");
  const RenditionSpec other = f.Rendition("o", 160, 90, "mp4");
  auto g = RenditionTemplate::Graph(f.Base(), {r, other});
  ASSERT_TRUE(g.ok());
  auto s = f.engine->CreateSession(*g);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  TranscodeController ctl(*f.engine, *session, f.Base(), {r, other});
  ASSERT_TRUE(session->Start().ok());

  const FilterSpec box{"box", "drawbox=x=0:y=0:w=80:h=40:color=white:t=fill"};
  // [frame index at insert, frame index at remove) per round.
  std::vector<std::pair<std::int64_t, std::int64_t>> windows;
  const ge::TopologyVersion v0 = session->topology_version();
  for (int round = 0; round < 3; ++round) {
    while (f.PacketsOut(session, "r.r.venc") < 20 + 40 * round) f.Turns(session, 1);
    const std::int64_t at_insert = f.PacketsOut(session, "r.r.venc");
    auto ins = ctl.InsertFilter("r", box);
    ASSERT_TRUE(ins.ok()) << ins.status().ToString();
    auto rec = ctl.Wait(*ins, std::chrono::seconds(10));
    ASSERT_TRUE(rec.ok()) << rec.status().ToString();
    EXPECT_EQ(rec->state, ge::OperationState::kSucceeded) << rec->result.ToString();
    ASSERT_EQ(ctl.Filters("r").size(), 1U);
    EXPECT_EQ(ctl.Filters("r")[0], box);
    EXPECT_NE(session->current_topology()->FindNode("r.r.f.box"), nullptr);
    // A second insert of the same id is rejected without touching the graph.
    EXPECT_EQ(ctl.InsertFilter("r", box).status().code(), GE_STATUS_ALREADY_EXISTS);

    while (f.PacketsOut(session, "r.r.venc") < 40 + 40 * round) f.Turns(session, 1);
    const std::int64_t at_remove = f.PacketsOut(session, "r.r.venc");
    auto rm = ctl.RemoveFilter("r", "box", /*drain=*/round % 2 == 0);
    ASSERT_TRUE(rm.ok()) << rm.status().ToString();
    auto rrec = ctl.Wait(*rm, std::chrono::seconds(10));
    ASSERT_TRUE(rrec.ok()) << rrec.status().ToString();
    EXPECT_EQ(rrec->state, ge::OperationState::kSucceeded) << rrec->result.ToString();
    EXPECT_TRUE(ctl.Filters("r").empty());
    EXPECT_EQ(session->current_topology()->FindNode("r.r.f.box"), nullptr);
    EXPECT_EQ(ctl.RemoveFilter("r", "box").status().code(), GE_STATUS_NOT_FOUND);
    windows.emplace_back(at_insert, at_remove);
  }
  // Six published versions: insert + remove per round.
  EXPECT_EQ(session->topology_version(), v0 + 6);
  EXPECT_EQ(ctl.InsertFilter("nope", box).status().code(), GE_STATUS_NOT_FOUND);
  EXPECT_EQ(ctl.InsertFilter("r", FilterSpec{"bad", "nosuchfilter"}).status().code(), GE_STATUS_INVALID_ARGUMENT);

  ASSERT_TRUE(f.RunToStop(session));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();
  EXPECT_EQ(ctl.stats().node_failures, 0U);

  const ProbeResult pr = Probe(r.output_path);
  EXPECT_EQ(pr.video_packets, 150) << pr.ToJson().Serialize();  // TR-C-1: nothing lost across six splices
  EXPECT_TRUE(pr.trailer_ok);
  EXPECT_TRUE(pr.first_video_is_key);
  for (std::size_t i = 1; i < pr.video_pts_ms.size(); ++i) {
    EXPECT_GT(pr.video_pts_ms[i], pr.video_pts_ms[i - 1]) << "pts not monotonic at " << i;
  }
  const ProbeResult po = Probe(other.output_path);
  EXPECT_EQ(po.video_packets, 150) << po.ToJson().Serialize();
  EXPECT_TRUE(po.trailer_ok);
  // The box is bright strictly inside each window and dark well outside it
  // (a few frames of slack for the packets in flight between scale and the
  // encoder when the splice landed).
  for (const auto& [ins, rm] : windows) {
    const double inside = AverageLuma(r.output_path, ins + 8, 0, 0, 80, 40);
    const double after = AverageLuma(r.output_path, rm + 8, 0, 0, 80, 40);
    const double before = AverageLuma(r.output_path, ins - 6, 0, 0, 80, 40);
    EXPECT_GT(inside, 200) << "window [" << ins << "," << rm << ") inside=" << inside;
    EXPECT_LT(after, 200) << "window [" << ins << "," << rm << ") after=" << after;
    EXPECT_LT(before, 200) << "window [" << ins << "," << rm << ") before=" << before;
  }
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}

// Hot update of the chain (TR-U-1 for filters): the box moves from the
// top-left to the top-right corner without a topology change.
TEST(MediaTranscodeTest, FilterHotUpdateMovesBoxWithoutTopologyChange) {
  Fixture f("filterhot", 90);
  const RenditionSpec r = f.Rendition("r", 320, 180, "flv");
  auto g = RenditionTemplate::Graph(f.Base(), {r});
  ASSERT_TRUE(g.ok());
  auto s = f.engine->CreateSession(*g);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  TranscodeController ctl(*f.engine, *session, f.Base(), {r});
  ASSERT_TRUE(session->Start().ok());

  auto ins = ctl.InsertFilter("r", FilterSpec{"box", "drawbox=x=0:y=0:w=80:h=40:color=white:t=fill"});
  ASSERT_TRUE(ins.ok()) << ins.status().ToString();
  auto rec = ctl.Wait(*ins, std::chrono::seconds(10));
  ASSERT_TRUE(rec.ok()) << rec.status().ToString();
  EXPECT_EQ(rec->state, ge::OperationState::kSucceeded) << rec->result.ToString();
  const ge::TopologyVersion v1 = session->topology_version();

  while (f.PacketsOut(session, "r.r.venc") < 40) f.Turns(session, 1);
  const std::int64_t at_update = f.PacketsOut(session, "r.r.venc");
  auto up = ctl.UpdateFilter("r", "box", "drawbox=x=240:y=0:w=80:h=40:color=white:t=fill");
  ASSERT_TRUE(up.ok()) << up.status().ToString();
  EXPECT_EQ(ctl.Filters("r")[0].chain, "drawbox=x=240:y=0:w=80:h=40:color=white:t=fill");
  EXPECT_EQ(ctl.UpdateFilter("r", "box", "nosuchfilter").status().code(), GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(ctl.UpdateFilter("r", "nope", "null").status().code(), GE_STATUS_NOT_FOUND);

  ASSERT_TRUE(f.RunToStop(session));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();
  EXPECT_EQ(session->topology_version(), v1);
  const ProbeResult pr = Probe(r.output_path);
  EXPECT_EQ(pr.video_packets, 90);
  EXPECT_TRUE(pr.trailer_ok);
  EXPECT_GT(AverageLuma(r.output_path, at_update - 6, 0, 0, 80, 40), 200);
  EXPECT_LT(AverageLuma(r.output_path, at_update - 6, 240, 0, 80, 40), 200);
  EXPECT_LT(AverageLuma(r.output_path, 85, 0, 0, 80, 40), 200);
  EXPECT_GT(AverageLuma(r.output_path, 85, 240, 0, 80, 40), 200);
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

// GM-3 batch reuse: three inputs through one transcode template on one
// engine. The template is validated/negotiated once; each item is its own
// session (isolation), each output independently playable.
TEST(MediaTranscodeTest, BatchTemplateTranscodesSeveralInputs) {
  Fixture f("batch", 45);
  // Two extra inputs with different frame counts.
  std::vector<std::string> inputs{f.input};
  std::vector<int> frames{45, 30, 60};
  for (int i = 1; i < 3; ++i) {
    SampleSpec spec;
    spec.path = (f.dir / ("in" + std::to_string(i) + ".mp4")).string();
    spec.frames = frames[static_cast<std::size_t>(i)];
    auto s = GenerateSample(spec);
    ASSERT_TRUE(s.ok()) << s.status().ToString();
    inputs.push_back(spec.path);
  }

  RenditionTemplate::BaseOptions base;
  base.input_path = "${input}";
  RenditionSpec r;
  r.id = "p180";
  r.width = 320;
  r.height = 180;
  r.bitrate_kbps = 500;
  r.gop = 30;
  r.output_path = "${output}";
  auto g = RenditionTemplate::Graph(base, {r});
  ASSERT_TRUE(g.ok()) << g.status().ToString();
  auto tmpl = ge::GraphTemplate::Create(std::move(*g), {ge::TemplateParameter{.name = "input"},
                                                        ge::TemplateParameter{.name = "output"}});
  ASSERT_TRUE(tmpl.ok()) << tmpl.status().ToString();
  ASSERT_TRUE(f.engine->PrevalidateTemplate(*tmpl).ok());

  ge::BatchOptions o;
  o.concurrency = 2;
  ge::BatchRunner runner(*f.engine, std::move(*tmpl), o);
  std::vector<ge::BatchItem> items;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    items.push_back(ge::BatchItem{
        "v" + std::to_string(i),
        ge::JsonValue(ge::JsonObject{{"input", ge::JsonValue(inputs[i])},
                                     {"output", ge::JsonValue(f.Out(("batch" + std::to_string(i)).c_str(), "flv"))}})});
  }
  auto report = runner.Run(items);
  ASSERT_TRUE(report.ok()) << report.status().ToString();
  ASSERT_TRUE(report->ok()) << [&] {
    std::string all;
    for (const auto& it : report->items) all += it.name + ": " + it.status.ToString() + "; ";
    return all;
  }();
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const ProbeResult pr = Probe(f.Out(("batch" + std::to_string(i)).c_str(), "flv"));
    EXPECT_EQ(pr.video_packets, frames[i]) << i;
    EXPECT_TRUE(pr.first_video_is_key) << i;
    EXPECT_TRUE(pr.trailer_ok) << i;
  }
  EXPECT_TRUE(f.engine->Sessions().empty());
}

// Long-video splitting, parallel flavour: the same template with
// start_ms/end_ms parameters cuts one input into slices; each slice is an
// independent session/file starting at a keyframe.
TEST(MediaTranscodeTest, SliceTemplateSplitsLongInputInParallel) {
  Fixture f("slices", 90, 30);  // 3s at 30fps, gop 30 => keyframes at 0/1/2s
  RenditionTemplate::BaseOptions base;
  base.input_path = "${input}";
  base.audio = false;
  RenditionSpec r;
  r.id = "p180";
  r.width = 320;
  r.height = 180;
  r.bitrate_kbps = 500;
  r.gop = 30;
  r.audio = false;
  r.output_path = "${output}";
  auto g = RenditionTemplate::Graph(base, {r});
  ASSERT_TRUE(g.ok()) << g.status().ToString();
  ge::GraphSpec skeleton = std::move(*g);
  ge::NodeSpec* demux = skeleton.FindNode(RenditionTemplate::kDemux);
  ASSERT_NE(demux, nullptr);
  demux->options.mutable_object().emplace("start_ms", ge::JsonValue("${start_ms}"));
  demux->options.mutable_object().emplace("end_ms", ge::JsonValue("${end_ms}"));
  auto tmpl = ge::GraphTemplate::Create(std::move(skeleton),
                                        {ge::TemplateParameter{.name = "input"}, ge::TemplateParameter{.name = "output"},
                                         ge::TemplateParameter{.name = "start_ms"}, ge::TemplateParameter{.name = "end_ms"}});
  ASSERT_TRUE(tmpl.ok()) << tmpl.status().ToString();

  ge::BatchOptions o;
  o.concurrency = 3;
  ge::BatchRunner runner(*f.engine, std::move(*tmpl), o);
  std::vector<ge::BatchItem> items;
  for (int i = 0; i < 3; ++i) {
    items.push_back(ge::BatchItem{
        "s" + std::to_string(i),
        ge::JsonValue(ge::JsonObject{{"input", ge::JsonValue(f.input)},
                                     {"output", ge::JsonValue(f.Out(("slice" + std::to_string(i)).c_str(), "mp4"))},
                                     {"start_ms", ge::JsonValue(i * 1000)},
                                     {"end_ms", ge::JsonValue(i == 2 ? 0 : (i + 1) * 1000)}})});
  }
  auto report = runner.Run(items);
  ASSERT_TRUE(report.ok()) << report.status().ToString();
  ASSERT_TRUE(report->ok()) << [&] {
    std::string all;
    for (const auto& it : report->items) all += it.name + ": " + it.status.ToString() + "; ";
    return all;
  }();
  std::int64_t total = 0;
  for (int i = 0; i < 3; ++i) {
    const ProbeResult pr = Probe(f.Out(("slice" + std::to_string(i)).c_str(), "mp4"));
    EXPECT_GT(pr.video_packets, 0) << i;
    EXPECT_TRUE(pr.first_video_is_key) << i;
    EXPECT_TRUE(pr.trailer_ok) << i;
    total += pr.video_packets;
  }
  // Slices start on the keyframe at/before start_ms and end at end_ms:
  // with gop == fps the boundaries are exact and nothing is lost.
  EXPECT_EQ(total, 90);
}

// Long-video splitting, single-session flavour: segment_duration_ms on the
// encoder + output_pattern on the mux produce N independently playable
// files from one uninterrupted pipeline (decoder/encoder never reopen).
TEST(MediaTranscodeTest, SegmentedMuxRotatesFilesOnEncoderBoundaries) {
  Fixture f("segments", 90, 30);  // 3s
  RenditionTemplate::BaseOptions base = f.Base();
  RenditionSpec r = f.Rendition("seg", 320, 180, "mp4");
  r.gop = 90;  // segments come from segment_duration_ms, not the gop
  auto g = RenditionTemplate::Graph(base, {r});
  ASSERT_TRUE(g.ok());
  ge::GraphSpec spec = std::move(*g);
  const RenditionNodeIds ids = RenditionTemplate::Ids("seg");
  ge::NodeSpec* venc = spec.FindNode(ids.venc);
  ASSERT_NE(venc, nullptr);
  venc->options.mutable_object().emplace("segment_duration_ms", ge::JsonValue(1000));
  ge::NodeSpec* mux = spec.FindNode(ids.mux);
  ASSERT_NE(mux, nullptr);
  mux->options.mutable_object().erase("output_path");
  mux->options.mutable_object().emplace("output_pattern", ge::JsonValue((f.dir / "seg_%03d.mp4").string()));
  mux->options.mutable_object().emplace("container", ge::JsonValue("mp4"));

  std::vector<std::string> segment_paths;
  std::mutex seg_mutex;
  const ge::SubscriptionId sub = f.engine->events().Subscribe(
      ge::EventFilter{.types = {std::string(kEventMediaSegment)}}, [&](const ge::Event& e) {
        std::lock_guard lock(seg_mutex);
        segment_paths.push_back(e.detail.GetString("path").value_or(""));
      });

  auto s = f.engine->CreateSession(spec);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  ASSERT_TRUE(session->Start().ok());
  ASSERT_TRUE(f.RunToStop(session));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();
  f.engine->events().Cancel(sub);

  std::int64_t total = 0;
  for (int i = 0; i < 3; ++i) {
    const std::string path = (f.dir / ("seg_00" + std::to_string(i) + ".mp4")).string();
    const ProbeResult pr = Probe(path);
    EXPECT_EQ(pr.video_packets, 30) << path;  // frame-accurate 1s boundaries
    EXPECT_TRUE(pr.first_video_is_key) << path;
    EXPECT_TRUE(pr.trailer_ok) << path;
    EXPECT_TRUE(pr.read_to_eof) << path;
    EXPECT_GT(pr.audio_packets, 0) << path;
    // Timestamps restart near zero in every segment.
    EXPECT_LT(pr.first_video_pts_ms, 100) << path;
    total += pr.video_packets;
  }
  EXPECT_EQ(total, 90);
  EXPECT_FALSE(fs::exists(f.dir / "seg_003.mp4"));
  {
    std::lock_guard lock(seg_mutex);
    EXPECT_EQ(segment_paths.size(), 2U);  // two rotations after the first file
  }
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}

// The user-facing variant of the filter scene: a drawtext overlay (needs
// libfreetype + a font; skipped where either is missing) toggled on and
// off a realtime rendition on the threaded engine, with the file checked
// for continuity afterwards.
TEST(MediaTranscodeTest, ThreadedRealtimeDrawtextToggle) {
  const std::string text = "drawtext=text='LIVE':x=8:y=8:fontsize=28:fontcolor=white:box=1:boxcolor=black@0.5";
  if (ge::Status s = ValidateFilterChain(text); !s.ok()) GTEST_SKIP() << "drawtext unavailable: " << s.ToString();
  Fixture f("drawtext", 90, 30, /*threads=*/true);
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
  for (int round = 0; round < 3; ++round) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto ins = ctl.InsertFilter("a", FilterSpec{"live", text});
    ASSERT_TRUE(ins.ok()) << ins.status().ToString();
    auto rec = ctl.Wait(*ins, std::chrono::seconds(5));
    ASSERT_TRUE(rec.ok()) << rec.status().ToString();
    EXPECT_EQ(rec->state, ge::OperationState::kSucceeded) << rec->result.ToString();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto rm = ctl.RemoveFilter("a", "live", /*drain=*/round != 1);
    ASSERT_TRUE(rm.ok()) << rm.status().ToString();
    auto rrec = ctl.Wait(*rm, std::chrono::seconds(5));
    ASSERT_TRUE(rrec.ok()) << rrec.status().ToString();
    EXPECT_EQ(rrec->state, ge::OperationState::kSucceeded) << rrec->result.ToString();
  }
  ASSERT_TRUE(session->WaitStopped(std::chrono::seconds(10)));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();
  EXPECT_EQ(ctl.stats().node_failures, 0U);
  const ProbeResult a = Probe(r0.output_path);
  EXPECT_EQ(a.video_packets, 90) << a.ToJson().Serialize();
  EXPECT_TRUE(a.trailer_ok);
  for (std::size_t i = 1; i < a.video_pts_ms.size(); ++i) {
    EXPECT_GT(a.video_pts_ms[i], a.video_pts_ms[i - 1]) << "pts not monotonic at " << i;
  }
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}
