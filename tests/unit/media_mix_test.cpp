#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <ge/cpp/engine.h>
#include <ge/media/mix.h>
#include <ge/media/mix_controller.h>
#include <ge/media/operators.h>
#include <ge/media/testing.h>

// P7 acceptance (docs/04): N-way composition + audio mix through the Engine.
// Inputs are generated with libavfilter; outputs re-read with libavformat.

namespace {

namespace fs = std::filesystem;
using namespace ge::media;

fs::path TempDir(const char* name) {
  const fs::path dir = fs::temp_directory_path() / ("ge_mix_" + std::string(name) + "_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

struct Fixture {
  fs::path dir;
  std::unique_ptr<ge::Engine> engine;

  explicit Fixture(const char* name) : dir(TempDir(name)) {
    ge::EngineConfig c;
    c.cpu_threads = 0;
    c.watchdog_thread = false;
    c.builtin_operators = MakeMediaOperatorFactory();
    auto e = ge::Engine::Create(std::move(c));
    EXPECT_TRUE(e.ok()) << e.status().ToString();
    engine = std::move(*e);
  }
  ~Fixture() {
    engine.reset();
    if (!::getenv("GE_KEEP")) fs::remove_all(dir);
  }

  std::string Input(const char* name, int frames = 60, bool audio = true) {
    const std::string path = (dir / (std::string(name) + ".mp4")).string();
    SampleSpec spec;
    spec.path = path;
    spec.frames = frames;
    spec.fps = 30;
    spec.gop = 30;
    spec.audio = audio;
    auto s = GenerateSample(spec);
    EXPECT_TRUE(s.ok()) << s.status().ToString();
    return path;
  }

  MixSpec Member(const char* id, const std::string& path) {
    MixSpec m;
    m.member_id = id;
    m.video_path = path;
    return m;
  }

  MixResultSpec Result(const char* name, const char* container = "mp4") {
    MixResultSpec r;
    r.output_path = (dir / (std::string(name) + "." + container)).string();
    r.container = container;
    return r;
  }

  bool RunToStop(ge::Session* s, int max_turns = 400000) {
    for (int i = 0; i < max_turns; ++i) {
      if (s->WaitStopped(std::chrono::milliseconds(0))) return true;
      (void)s->PumpMutations();
      engine->Tick();
    }
    return false;
  }
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
// Layout maths (no engine)
// ---------------------------------------------------------------------------

TEST(MixLayoutTest, GridCellsCoverTheCanvasWithoutOverlap) {
  for (const int count : {1, 2, 3, 4, 6, 9, 12, 16}) {
    std::int64_t area = 0;
    for (int i = 0; i < count; ++i) {
      const MixRect c = GridCell(i, count, 1280, 720, 0);
      EXPECT_GT(c.w, 0) << count << "/" << i;
      EXPECT_GT(c.h, 0) << count << "/" << i;
      EXPECT_GE(c.x, 0);
      EXPECT_GE(c.y, 0);
      EXPECT_LE(c.x + c.w, 1280);
      EXPECT_LE(c.y + c.h, 720);
      area += static_cast<std::int64_t>(c.w) * c.h;
      for (int j = 0; j < i; ++j) {
        const MixRect o = GridCell(j, count, 1280, 720, 0);
        const bool disjoint = c.x + c.w <= o.x || o.x + o.w <= c.x || c.y + c.h <= o.y || o.y + o.h <= c.y;
        EXPECT_TRUE(disjoint) << count << ": " << i << " vs " << j;
      }
    }
    // Integer division may shave a pixel per row/column, never more than 5%.
    EXPECT_GE(area, static_cast<std::int64_t>(1280) * 720 * 95 / 100) << count;
  }
}

TEST(MixLayoutTest, ExplicitRectsWinAndTheRestGridsBelow) {
  std::vector<SlotSpec> slots(3);
  slots[0].member_id = "host";
  slots[0].rect = MixRect{0, 0, 1280, 360};
  slots[1].member_id = "a";
  slots[2].member_id = "b";
  const std::vector<SlotSpec> out = ResolveLayout(slots, 1280, 720, 0);
  EXPECT_EQ(out[0].rect, (MixRect{0, 0, 1280, 360}));  // kept (MX-L-4)
  EXPECT_GE(out[1].rect.y, 360);                       // gridded into the free band
  EXPECT_GE(out[2].rect.y, 360);
  EXPECT_GT(out[1].rect.w, 0);
  EXPECT_NE(out[1].rect.x, out[2].rect.x);
}

TEST(MixTemplateTest, GraphWiresEveryMemberIntoItsOwnPort) {
  LayoutSpec layout;
  MixOptions options;
  MixResultSpec result;
  result.output_path = "out.flv";
  std::vector<MixSpec> members;
  for (const char* id : {"alice", "bob", "carol"}) {
    MixSpec m;
    m.member_id = id;
    m.video_path = "in.mp4";
    members.push_back(std::move(m));
  }
  auto g = MixTemplate::Graph(layout, options, members, result);
  ASSERT_TRUE(g.ok()) << g.status().ToString();
  // compose venc mux amix aenc + 3 * (src dec asrc adec)
  EXPECT_EQ(g->nodes().size(), 5U + 12U);
  EXPECT_NE(g->FindEdge("e.alice.main"), nullptr);
  EXPECT_NE(g->FindEdge("e.bob.s1"), nullptr);
  EXPECT_NE(g->FindEdge("e.carol.s2"), nullptr);

  // Port assignment is stable and validation catches abuse.
  EXPECT_EQ(MixTemplate::MemberPort(0), "main");
  EXPECT_EQ(MixTemplate::MemberPort(3), "s3");
  MixSpec bad;
  bad.member_id = "x y";
  bad.video_path = "in.mp4";
  EXPECT_EQ(MixTemplate::ValidateMember(bad).code(), GE_STATUS_INVALID_ARGUMENT);
  MixSpec no_path;
  no_path.member_id = "ok";
  EXPECT_EQ(MixTemplate::ValidateMember(no_path).code(), GE_STATUS_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// End to end
// ---------------------------------------------------------------------------

TEST(MixTest, TwoMembersComposeToOnePlayableOutput) {
  Fixture f("basic");
  const std::string a = f.Input("a", 60);
  const std::string b = f.Input("b", 60);
  LayoutSpec layout;
  layout.width = 640;
  layout.height = 360;
  MixOptions options;
  const MixResultSpec result = f.Result("mix");
  const std::vector<MixSpec> members = {f.Member("alice", a), f.Member("bob", b)};
  auto g = MixTemplate::Graph(layout, options, members, result);
  ASSERT_TRUE(g.ok()) << g.status().ToString();
  auto s = f.engine->CreateSession(*g);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  ASSERT_TRUE(session->Start().ok());
  ASSERT_TRUE(f.RunToStop(session));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();

  const ProbeResult out = Probe(result.output_path);
  EXPECT_EQ(out.width, 640);
  EXPECT_EQ(out.height, 360);
  EXPECT_GT(out.video_packets, 30) << out.ToJson().Serialize();  // ~2s at 30fps
  EXPECT_GT(out.audio_packets, 0);                               // mixed audio present
  EXPECT_TRUE(out.first_video_is_key);
  EXPECT_TRUE(out.read_to_eof);
  EXPECT_TRUE(out.trailer_ok);
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}

TEST(MixTest, MemberJoinsAndLeavesWhileOutputKeepsRunning) {
  Fixture f("dynamic");
  const std::string a = f.Input("a", 150);
  const std::string b = f.Input("b", 150);
  const std::string c = f.Input("c", 150);
  LayoutSpec layout;
  layout.width = 640;
  layout.height = 360;
  MixOptions options;
  const MixResultSpec result = f.Result("mix", "flv");
  std::vector<MixSpec> members = {f.Member("alice", a), f.Member("bob", b)};
  auto g = MixTemplate::Graph(layout, options, members, result);
  ASSERT_TRUE(g.ok()) << g.status().ToString();
  auto s = f.engine->CreateSession(*g);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  MixController ctl(*f.engine, *session, layout, options, members);
  ASSERT_TRUE(session->Start().ok());

  // Let the mix settle, then a third member joins (MX-I-1).
  while (f.PacketsOut(session, "venc") < 20) f.Turns(session, 1);
  auto join = ctl.AddMember(f.Member("carol", c));
  ASSERT_TRUE(join.ok()) << join.status().ToString();
  auto rec = ctl.Wait(*join, std::chrono::seconds(20));
  ASSERT_TRUE(rec.ok()) << rec.status().ToString();
  EXPECT_EQ(rec->state, ge::OperationState::kSucceeded) << rec->result.ToString();
  EXPECT_EQ(ctl.Members().size(), 3U);
  EXPECT_EQ(ctl.PortOf("carol"), "s2");

  // bob leaves (MX-I-2); alice and carol keep the output alive.
  while (f.PacketsOut(session, "venc") < 40) f.Turns(session, 1);
  auto leave = ctl.RemoveMember("bob");
  ASSERT_TRUE(leave.ok()) << leave.status().ToString();
  auto rec2 = ctl.Wait(*leave, std::chrono::seconds(20));
  ASSERT_TRUE(rec2.ok()) << rec2.status().ToString();
  EXPECT_EQ(rec2->state, ge::OperationState::kSucceeded) << rec2->result.ToString();
  EXPECT_EQ(ctl.Members().size(), 2U);
  EXPECT_TRUE(ctl.PortOf("bob").empty());

  ASSERT_TRUE(f.RunToStop(session));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();
  const ProbeResult out = Probe(result.output_path);
  EXPECT_GT(out.video_packets, 60) << out.ToJson().Serialize();
  EXPECT_TRUE(out.first_video_is_key);
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}

TEST(MixTest, LayoutMoveAndGainAreHotUpdates) {
  Fixture f("hot");
  const std::string a = f.Input("a", 120);
  const std::string b = f.Input("b", 120);
  LayoutSpec layout;
  layout.width = 640;
  layout.height = 360;
  MixOptions options;
  const MixResultSpec result = f.Result("mix");
  const std::vector<MixSpec> members = {f.Member("alice", a), f.Member("bob", b)};
  auto g = MixTemplate::Graph(layout, options, members, result);
  ASSERT_TRUE(g.ok()) << g.status().ToString();
  auto s = f.engine->CreateSession(*g);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  MixController ctl(*f.engine, *session, layout, options, members);
  ASSERT_TRUE(session->Start().ok());
  while (f.PacketsOut(session, "venc") < 10) f.Turns(session, 1);

  // bob to the big window (MX-L-3), alice muted (MX-A-3): both are
  // parameter updates, no topology version change.
  const ge::JsonValue before = session->Snapshot();
  const std::int64_t topo_before = before.GetInteger("topology_version").value_or(-1);
  auto move = ctl.MoveMember("bob", MixRect{0, 0, 640, 360}, MixFit::kContain, 5);
  ASSERT_TRUE(move.ok()) << move.status().ToString();
  auto gain = ctl.SetGain("alice", 0);
  ASSERT_TRUE(gain.ok()) << gain.status().ToString();
  EXPECT_EQ(ctl.SetGain("ghost", 500).status().code(), GE_STATUS_NOT_FOUND);
  EXPECT_EQ(ctl.SetGain("alice", 20000).status().code(), GE_STATUS_INVALID_ARGUMENT);

  ASSERT_TRUE(f.RunToStop(session));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();
  const ge::JsonValue after = session->Snapshot();
  EXPECT_EQ(after.GetInteger("topology_version").value_or(-2), topo_before);
  const ProbeResult out = Probe(result.output_path);
  EXPECT_GT(out.video_packets, 30);
  EXPECT_TRUE(out.trailer_ok);
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}

TEST(MixTest, UnevenInputsFreezeCompensationKeepsTicking) {
  Fixture f("freeze");
  // alice is twice as long as bob: after bob's file ends his slot freezes
  // (on_missing=freeze) while alice keeps driving the reference timeline.
  const std::string a = f.Input("a", 120);
  const std::string b = f.Input("b", 40);
  LayoutSpec layout;
  layout.width = 640;
  layout.height = 360;
  MixOptions options;
  options.on_missing = "freeze";
  const MixResultSpec result = f.Result("mix");
  const std::vector<MixSpec> members = {f.Member("alice", a), f.Member("bob", b)};
  auto g = MixTemplate::Graph(layout, options, members, result);
  ASSERT_TRUE(g.ok()) << g.status().ToString();
  auto s = f.engine->CreateSession(*g);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ge::Session* session = *s;
  ASSERT_TRUE(session->Start().ok());
  ASSERT_TRUE(f.RunToStop(session));
  EXPECT_EQ(session->state(), ge::SessionState::kStopped) << session->failure().ToString();
  const ProbeResult out = Probe(result.output_path);
  // The output is driven by alice (~4s), well past bob's 40 frames.
  EXPECT_GT(out.video_packets, 80) << out.ToJson().Serialize();
  EXPECT_TRUE(out.read_to_eof);
  EXPECT_TRUE(f.engine->DestroySession(session->id()).ok());
}
