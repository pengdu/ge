#include <ge/cpp/engine.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <ge/c/ge_engine.h>
#include <ge/cpp/graph_spec_json.h>

namespace {

namespace fs = std::filesystem;

fs::path PluginDir() {
  const char* env = std::getenv("GE_PLUGIN_DIR");
  if (env != nullptr) return fs::path(env);
  return fs::path(GE_PLUGIN_DIR);
}

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

// Inline executor, host-driven watchdog: every test drives the engine with
// Pump() so results are deterministic.
ge::EngineConfig InlineConfig() {
  ge::EngineConfig c;
  c.plugin_search_paths = {PluginDir()};
  c.cpu_threads = 0;
  c.watchdog_thread = false;
  c.default_drain_timeout = std::chrono::milliseconds(500);
  return c;
}

void Pump(ge::Engine& e, ge::Session* s) {
  if (s != nullptr) (void)s->PumpMutations();
  e.Tick();
}

bool RunToStop(ge::Engine& e, ge::Session* s, int max_iterations = 100000) {
  for (int i = 0; i < max_iterations; ++i) {
    if (s->WaitStopped(std::chrono::milliseconds(0))) return true;
    Pump(e, s);
  }
  return s->WaitStopped(std::chrono::milliseconds(0));
}

// Advances roughly one process call per iteration (RunPending would drain
// the whole stream because tasks re-enqueue themselves).
void RunSome(ge::Engine& e, ge::Session* s, int iterations) {
  for (int i = 0; i < iterations; ++i) {
    (void)s->PumpMutations();
    (void)e.executor().RunOne();
    s->Tick();
  }
}

ge::GraphSpec Linear(const char* name, int count, const char* pass_key = "Pass@1.0.0",
                     ge::JsonValue pass_options = ge::JsonValue(ge::JsonObject{})) {
  ge::GraphBuilder g(name);
  auto src = g.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(count)}}));
  auto pass = g.AddNode(Op(pass_key), "pass", std::move(pass_options));
  auto sink = g.AddNode(Op("Sink@1.0.0"), "sink");
  g.Connect(src.port("out"), pass.port("in"), {.id = "e0"});
  g.Connect(pass.port("out"), sink.port("in"), {.id = "e1"});
  return *g.Build();
}

std::int64_t Metric(const ge::JsonValue& snapshot, const char* node, const char* metric) {
  for (const ge::JsonValue& n : snapshot.Find("nodes")->as_array()) {
    if (n.GetString("id").value_or("") == node) {
      return n.Find("metrics")->GetInteger(metric).value_or(-1);
    }
  }
  return -1;
}

std::string NodeOp(const ge::JsonValue& snapshot, const char* node) {
  for (const ge::JsonValue& n : snapshot.Find("nodes")->as_array()) {
    if (n.GetString("id").value_or("") == node) return n.GetString("op").value_or("");
  }
  return "";
}

// ---------------------------------------------------------------------------
// Engine (C++): plugin lifecycle through the host API.
// ---------------------------------------------------------------------------

TEST(EngineTest, ConfigFromJsonValidatesFields) {
  auto c = ge::EngineConfig::FromJson(
      R"({"paths":["/tmp"],"host_dependencies":{"tensorrt":"9.9.1"}})",
      R"({"cpu_threads":2,"buffer_pool_bytes":1024,"drain_timeout_ms":250})",
      R"({"event_queue_capacity":64,"observer_threads":1,"watchdog_period_ms":5})");
  ASSERT_TRUE(c.ok()) << c.status().ToString();
  EXPECT_EQ(c->plugin_search_paths.size(), 1U);
  EXPECT_EQ(c->host_dependencies.at("tensorrt"), "9.9.1");
  EXPECT_EQ(c->cpu_threads, 2U);
  EXPECT_EQ(c->buffer_pool.max_cached_bytes, 1024U);
  EXPECT_EQ(c->default_drain_timeout, std::chrono::milliseconds(250));
  EXPECT_EQ(c->events.queue_capacity, 64U);
  EXPECT_EQ(c->watchdog_period, std::chrono::milliseconds(5));
  // Bare array form for search paths.
  EXPECT_TRUE(ge::EngineConfig::FromJson(R"(["/tmp"])", nullptr, nullptr).ok());
  EXPECT_FALSE(ge::EngineConfig::FromJson(R"({"nope":1})", nullptr, nullptr).ok());
  EXPECT_FALSE(ge::EngineConfig::FromJson(nullptr, R"({"cpu_threads":-1})", nullptr).ok());
  EXPECT_FALSE(ge::EngineConfig::FromJson(nullptr, nullptr, R"({"event_queue_capacity":0})").ok());
  EXPECT_FALSE(ge::EngineConfig::FromJson("not json", nullptr, nullptr).ok());
  // A search path that is not a directory is refused at create.
  ge::EngineConfig bad = InlineConfig();
  bad.plugin_search_paths.emplace_back(PluginDir() / "does_not_exist");
  EXPECT_EQ(ge::Engine::Create(bad).status().code(), GE_STATUS_INVALID_ARGUMENT);
}

TEST(EngineTest, RejectedPluginsDoNotAffectLoadedOnes) {
  auto engine = ge::Engine::Create(InlineConfig());
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;
  auto v1 = e.LoadPlugin(PluginDir() / "sample_plugin.json");
  ASSERT_TRUE(v1.ok()) << v1.status().ToString();
  // Rejections: ABI major, manifest, dependency, symbol -- each with the
  // correct code and no effect on the registered plugin.
  struct Case {
    const char* manifest;
    ge_status_code code;
  };
  for (const Case& c : {Case{"bad_abi_major.json", GE_STATUS_PLUGIN_ABI_MISMATCH},
                        Case{"bad_manifest_abi.json", GE_STATUS_PLUGIN_ABI_MISMATCH},
                        Case{"bad_missing_library.json", GE_STATUS_PLUGIN_MANIFEST_INVALID},
                        Case{"bad_dependency.json", GE_STATUS_PLUGIN_MANIFEST_INVALID},
                        Case{"no_symbol.json", GE_STATUS_PLUGIN_ABI_MISMATCH},
                        Case{"bad_fingerprint.json", GE_STATUS_PLUGIN_ABI_MISMATCH}}) {
    auto r = e.LoadPlugin(PluginDir() / c.manifest);
    ASSERT_FALSE(r.ok()) << c.manifest;
    EXPECT_EQ(r.status().code(), c.code) << c.manifest << ": " << r.status().ToString();
    EXPECT_EQ(e.plugins().List().size(), 1U) << c.manifest;
    EXPECT_EQ(e.plugins().Get(v1->id)->state, ge::PluginState::kRegistered) << c.manifest;
  }
  // The loaded plugin still serves capability lookups and sessions.
  auto cap = e.GetCapability(Op("Pass@1.0.0"));
  ASSERT_TRUE(cap.ok());
  EXPECT_EQ(cap->description, "pass-through with gain");
  EXPECT_EQ(e.GetCapability(Op("Bad@1.0.0")).status().code(), GE_STATUS_NOT_FOUND);
  auto s = e.CreateSession(Linear("g", 20));
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ASSERT_TRUE((*s)->Start().ok());
  ASSERT_TRUE(RunToStop(e, *s));
  EXPECT_EQ(Metric((*s)->Snapshot(), "sink", "packets_in"), 20);
  EXPECT_TRUE(e.DestroySession((*s)->id()).ok());
  EXPECT_EQ(e.DestroySession(999).code(), GE_STATUS_NOT_FOUND);
}

TEST(EngineTest, TwoVersionsServeDifferentSessions) {
  auto engine = ge::Engine::Create(InlineConfig());
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;
  auto v1 = e.LoadPlugin(PluginDir() / "sample_plugin.json");
  auto v2 = e.LoadPlugin(PluginDir() / "sample_plugin_v2.json");
  ASSERT_TRUE(v1.ok() && v2.ok());
  auto s1 = e.CreateSession(Linear("one", 30, "Pass@1.0.0"));
  auto s2 = e.CreateSession(Linear("two", 30, "Pass@2.0.0"));
  ASSERT_TRUE(s1.ok() && s2.ok());
  EXPECT_EQ(e.Sessions().size(), 2U);
  EXPECT_EQ(e.FindSession((*s1)->id()), *s1);
  // v2 only replaces "pass"; src/sink of both sessions come from v1.
  EXPECT_EQ(e.plugins().Get(v1->id)->references, 5U);
  EXPECT_EQ(e.plugins().Get(v2->id)->references, 1U);
  ASSERT_TRUE((*s1)->Start().ok());
  ASSERT_TRUE((*s2)->Start().ok());
  ASSERT_TRUE(RunToStop(e, *s1));
  ASSERT_TRUE(RunToStop(e, *s2));
  EXPECT_EQ(Metric((*s1)->Snapshot(), "sink", "packets_in"), 30);
  EXPECT_EQ(Metric((*s2)->Snapshot(), "sink", "packets_in"), 30);
  EXPECT_EQ(NodeOp((*s1)->Snapshot(), "pass"), "Pass@1.0.0");
  EXPECT_EQ(NodeOp((*s2)->Snapshot(), "pass"), "Pass@2.0.0");
}

TEST(EngineTest, RetirePluginUnloadsLogicallyWhenReferencesDrop) {
  auto engine = ge::Engine::Create(InlineConfig());
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;
  std::vector<std::string> transitions;
  std::mutex mu;
  const ge::SubscriptionId sub = e.events().Subscribe(
      ge::EventFilter{.types = {"plugin_state"}}, [&](const ge::Event& ev) {
        std::lock_guard lock(mu);
        transitions.push_back(ev.detail.GetString("to").value_or(""));
      });
  auto v1 = e.LoadPlugin(PluginDir() / "sample_plugin.json");
  ASSERT_TRUE(v1.ok());
  auto s = e.CreateSession(Linear("g", 40));
  ASSERT_TRUE(s.ok());
  ASSERT_TRUE((*s)->Start().ok());
  RunSome(e, *s, 5);
  auto op = e.RetirePlugin(v1->id);
  ASSERT_TRUE(op.ok());
  EXPECT_EQ(e.plugins().Get(v1->id)->state, ge::PluginState::kRetiring);
  EXPECT_EQ(e.operations().Get(*op)->state, ge::OperationState::kRunning);
  // Retiring: no new nodes from this plugin, running ones keep going.
  auto refused = e.CreateSession(Linear("late", 5));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.status().code(), GE_STATUS_PLUGIN_RETIRED);
  ASSERT_TRUE(RunToStop(e, *s));
  EXPECT_EQ(Metric((*s)->Snapshot(), "sink", "packets_in"), 40);
  EXPECT_EQ(e.plugins().Get(v1->id)->state, ge::PluginState::kRetiring);  // session still holds refs
  ASSERT_TRUE(e.DestroySession((*s)->id()).ok());
  // References at zero -> logical unload; the handle stays open (no dlclose).
  const auto info = e.plugins().Get(v1->id);
  EXPECT_EQ(info->state, ge::PluginState::kLogicallyUnloaded);
  EXPECT_EQ(info->references, 0U);
  EXPECT_EQ(e.operations().Get(*op)->state, ge::OperationState::kSucceeded);
  EXPECT_EQ(e.plugins().Resolve(Op("Src@1.0.0")), std::nullopt);
  EXPECT_EQ(e.CreateSession(Linear("gone", 5)).status().code(), GE_STATUS_NOT_FOUND);
  // Retire twice is not an error state: the operation reports it.
  auto again = e.RetirePlugin(v1->id);
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(e.operations().Get(*again)->state, ge::OperationState::kFailed);
  EXPECT_EQ(e.RetirePlugin(4242).status().code(), GE_STATUS_NOT_FOUND);
  e.events().Flush();
  e.events().Cancel(sub);
  {
    std::lock_guard lock(mu);
    EXPECT_NE(std::find(transitions.begin(), transitions.end(), "retiring"), transitions.end());
    EXPECT_NE(std::find(transitions.begin(), transitions.end(), "logically_unloaded"), transitions.end());
    EXPECT_EQ(std::find(transitions.begin(), transitions.end(), "physically_unloaded"), transitions.end());
  }
}

TEST(EngineTest, RetireWithPhysicalUnloadAndImmediateUnload) {
  auto engine = ge::Engine::Create(InlineConfig());
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;
  auto v1 = e.LoadPlugin(PluginDir() / "sample_plugin.json");
  ASSERT_TRUE(v1.ok());
  // No references: completes synchronously, including dlclose.
  auto op = e.RetirePlugin(v1->id, {.request_physical_unload = true});
  ASSERT_TRUE(op.ok());
  const auto rec = e.operations().Get(*op);
  EXPECT_EQ(rec->state, ge::OperationState::kSucceeded) << rec->result.ToString();
  EXPECT_EQ(rec->detail.GetBool("physically_unloaded").value_or(false), true);
  EXPECT_EQ(e.plugins().Get(v1->id)->state, ge::PluginState::kPhysicallyUnloaded);
  // Same plugin id can be loaded again afterwards.
  auto v1b = e.LoadPlugin(PluginDir() / "sample_plugin.json");
  ASSERT_TRUE(v1b.ok()) << v1b.status().ToString();
  EXPECT_NE(v1b->id, v1->id);
}

TEST(EngineTest, UpgradeOperatorAggregatesPerSessionWithoutRollback) {
  auto engine = ge::Engine::Create(InlineConfig());
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;
  auto v1 = e.LoadPlugin(PluginDir() / "sample_plugin.json");
  auto v2 = e.LoadPlugin(PluginDir() / "sample_plugin_v2.json");
  ASSERT_TRUE(v1.ok() && v2.ok());
  // Session A upgrades fine. Session B is stopped before the upgrade, so its
  // mutation is refused: aggregate result is partial failure, A is not
  // rolled back and the old plugin stays retiring.
  auto a = e.CreateSession(Linear("a", 400));
  auto b = e.CreateSession(Linear("b", 10));
  auto c = e.CreateSession(Linear("c", 400, "Pass@2.0.0"));  // untouched: no old nodes
  ASSERT_TRUE(a.ok() && b.ok() && c.ok());
  // The inline executor is shared: finish b before a/c start so their
  // streams are still in flight at upgrade time.
  ASSERT_TRUE((*b)->Start().ok());
  ASSERT_TRUE(RunToStop(e, *b));
  EXPECT_EQ((*b)->state(), ge::SessionState::kStopped);
  ASSERT_TRUE((*a)->Start().ok());
  ASSERT_TRUE((*c)->Start().ok());
  RunSome(e, *a, 20);

  auto rep = e.UpgradeOperator(Op("Pass@1.0.0"), Op("Pass@2.0.0"));
  ASSERT_TRUE(rep.ok()) << rep.status().ToString();
  EXPECT_FALSE(rep->all_succeeded());
  EXPECT_FALSE(rep->unloaded);
  ASSERT_EQ(rep->sessions.size(), 2U);
  for (const ge::SessionUpgradeResult& r : rep->sessions) {
    EXPECT_EQ(r.nodes, std::vector<std::string>{"pass"});
    if (r.session == (*a)->id()) {
      EXPECT_TRUE(r.result.ok()) << r.result.ToString();
      EXPECT_NE(r.operation, 0U);
    } else {
      EXPECT_EQ(r.session, (*b)->id());
      EXPECT_FALSE(r.result.ok());
      EXPECT_EQ(r.operation, 0U);
    }
  }
  const auto rec = e.operations().Get(rep->operation);
  EXPECT_EQ(rec->state, ge::OperationState::kFailed);
  EXPECT_EQ(rec->detail.GetBool("all_succeeded").value_or(true), false);
  EXPECT_EQ(rec->detail.Find("sessions")->as_array().size(), 2U);
  // A really runs the new operator now and finishes the stream intact; the
  // old plugin stays retiring (still referenced, not rolled back).
  EXPECT_EQ(NodeOp((*a)->Snapshot(), "pass"), "Pass@2.0.0");
  EXPECT_EQ(e.plugins().Get(v1->id)->state, ge::PluginState::kRetiring);
  // Errors: unknown keys, identical keys, retiring replacement.
  EXPECT_EQ(e.UpgradeOperator(Op("Pass@1.0.0"), Op("Pass@1.0.0")).status().code(), GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(e.UpgradeOperator(Op("Nope@1.0.0"), Op("Pass@2.0.0")).status().code(), GE_STATUS_NOT_FOUND);
  EXPECT_EQ(e.UpgradeOperator(Op("Pass@2.0.0"), Op("Pass@1.0.0")).status().code(), GE_STATUS_PLUGIN_RETIRED);
  ASSERT_TRUE(RunToStop(e, *a));
  ASSERT_TRUE(RunToStop(e, *c));
  EXPECT_EQ(Metric((*a)->Snapshot(), "sink", "packets_in"), 400);
  EXPECT_EQ(Metric((*c)->Snapshot(), "sink", "packets_in"), 400);
  EXPECT_EQ(e.plugins().Get(v1->id)->state, ge::PluginState::kRetiring);
  // Old plugin refs (src/sink of a/b/c, old pass of b) vanish with the
  // sessions -> retiring + zero references == logical unload (12 §9.3).
  ASSERT_TRUE(e.DestroySession((*a)->id()).ok());
  ASSERT_TRUE(e.DestroySession((*b)->id()).ok());
  ASSERT_TRUE(e.DestroySession((*c)->id()).ok());
  e.Tick();
  EXPECT_EQ(e.plugins().Get(v1->id)->state, ge::PluginState::kLogicallyUnloaded);
  EXPECT_EQ(e.plugins().Get(v1->id)->references, 0U);
  EXPECT_EQ(e.plugins().Get(v2->id)->state, ge::PluginState::kActive);
}

TEST(EngineTest, UpgradeOperatorAllSucceededUnloadsOldPlugin) {
  auto engine = ge::Engine::Create(InlineConfig());
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;
  auto v1 = e.LoadPlugin(PluginDir() / "sample_plugin.json");
  auto v2 = e.LoadPlugin(PluginDir() / "sample_plugin_v2.json");
  ASSERT_TRUE(v1.ok() && v2.ok());
  // Both sessions use only v2 for src/sink is impossible (v2 registers all
  // three); so upgrade every operator type and check the old plugin is gone
  // once the last one is replaced.
  auto a = e.CreateSession(Linear("a", 300));
  auto b = e.CreateSession(Linear("b", 300));
  ASSERT_TRUE(a.ok() && b.ok());
  ASSERT_TRUE((*a)->Start().ok());
  ASSERT_TRUE((*b)->Start().ok());
  RunSome(e, *a, 10);
  RunSome(e, *b, 10);
  auto rep = e.UpgradeOperator(Op("Pass@1.0.0"), Op("Pass@2.0.0"));
  ASSERT_TRUE(rep.ok()) << rep.status().ToString();
  EXPECT_TRUE(rep->all_succeeded()) << rep->ToJson().Serialize();
  EXPECT_EQ(rep->sessions.size(), 2U);
  EXPECT_FALSE(rep->unloaded);  // src/sink still reference v1
  EXPECT_EQ(e.plugins().Get(v1->id)->state, ge::PluginState::kRetiring);
  EXPECT_EQ(e.operations().Get(rep->operation)->state, ge::OperationState::kSucceeded);
  // Retiring: the key still resolves (running graphs keep validating) but
  // new instances are refused.
  EXPECT_EQ(e.plugins().Resolve(Op("Pass@1.0.0")), v1->id);
  EXPECT_NE(e.plugins().Describe(Op("Pass@1.0.0")), nullptr);
  EXPECT_EQ(e.CreateSession(Linear("late", 3)).status().code(), GE_STATUS_PLUGIN_RETIRED);
  ASSERT_TRUE(RunToStop(e, *a));
  ASSERT_TRUE(RunToStop(e, *b));
  EXPECT_EQ(Metric((*a)->Snapshot(), "sink", "packets_in"), 300);
  EXPECT_EQ(Metric((*b)->Snapshot(), "sink", "packets_in"), 300);
  ASSERT_TRUE(e.DestroySession((*a)->id()).ok());
  ASSERT_TRUE(e.DestroySession((*b)->id()).ok());
  e.Tick();
  EXPECT_EQ(e.plugins().Get(v1->id)->state, ge::PluginState::kLogicallyUnloaded);
  EXPECT_EQ(e.plugins().Get(v1->id)->references, 0U);
}

TEST(EngineTest, DestructorStopsRunningSessionsAndReleasesPlugins) {
  std::shared_ptr<ge::HostBufferPool> pool;
  {
    auto engine = ge::Engine::Create(InlineConfig());
    ASSERT_TRUE(engine.ok());
    ge::Engine& e = **engine;
    pool = e.buffer_pool();
    ASSERT_TRUE(e.LoadPlugin(PluginDir() / "sample_plugin.json").ok());
    auto s = e.CreateSession(Linear("g", 100000));
    ASSERT_TRUE(s.ok());
    ASSERT_TRUE((*s)->Start().ok());
    RunSome(e, *s, 50);
    EXPECT_EQ((*s)->state(), ge::SessionState::kRunning);
  }
  EXPECT_EQ(pool->stats().live_buffers, 0U);
}

TEST(EngineTest, ThreadedEngineRunsPluginGraph) {
  ge::EngineConfig c = InlineConfig();
  c.cpu_threads = 2;
  c.watchdog_thread = true;
  c.watchdog_period = std::chrono::milliseconds(1);
  auto engine = ge::Engine::Create(c);
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;
  ASSERT_TRUE(e.LoadPlugin(PluginDir() / "sample_plugin.json").ok());
  ASSERT_TRUE(e.LoadPlugin(PluginDir() / "sample_plugin_v2.json").ok());
  auto s = e.CreateSession(Linear("t", 2000));
  ASSERT_TRUE(s.ok());
  ASSERT_TRUE((*s)->Start().ok());
  auto rep = e.UpgradeOperator(Op("Pass@1.0.0"), Op("Pass@2.0.0"));
  ASSERT_TRUE(rep.ok()) << rep.status().ToString();
  EXPECT_TRUE(rep->all_succeeded());
  ASSERT_TRUE((*s)->WaitStopped(std::chrono::seconds(30)));
  EXPECT_EQ(Metric((*s)->Snapshot(), "sink", "packets_in"), 2000);
  EXPECT_EQ(NodeOp((*s)->Snapshot(), "pass"), "Pass@2.0.0");
}

std::int64_t AsyncMetric(const ge::JsonValue& snapshot, const char* node, const char* metric) {
  for (const ge::JsonValue& n : snapshot.Find("nodes")->as_array()) {
    if (n.GetString("id").value_or("") != node) continue;
    const ge::JsonValue* a = n.Find("metrics")->Find("async");
    return a != nullptr ? a->GetInteger(metric).value_or(-1) : -1;
  }
  return -1;
}

// P5 through the C ABI: the sample plugin's AsyncPass submits and pushes
// completions via completion_push; the inline engine pumps the runtime.
TEST(EngineTest, InlineEngineRunsPluginAsyncOperator) {
  auto engine = ge::Engine::Create(InlineConfig());
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;
  ASSERT_TRUE(e.LoadPlugin(PluginDir() / "sample_plugin.json").ok());
  auto s = e.CreateSession(Linear("a", 300, "AsyncPass@1.0.0"));
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ASSERT_TRUE((*s)->Start().ok());
  ASSERT_TRUE(RunToStop(e, *s));
  const ge::JsonValue snap = (*s)->Snapshot();
  EXPECT_EQ(Metric(snap, "sink", "packets_in"), 300);
  EXPECT_EQ(AsyncMetric(snap, "pass", "submitted"), 300);
  EXPECT_EQ(AsyncMetric(snap, "pass", "completed"), 300);
  EXPECT_EQ(AsyncMetric(snap, "pass", "in_flight"), 0);
  EXPECT_EQ(AsyncMetric(snap, "pass", "orphan_completions"), 0);
  EXPECT_EQ(AsyncMetric(snap, "pass", "reorder_gaps"), 0);
  EXPECT_GT(AsyncMetric(snap, "pass", "batch_count"), 0);
  EXPECT_EQ(e.async_runtime().in_flight(), 0U);
}

// Same graph on real threads: executor workers submit, the AsyncRuntime
// worker consumes completions, and a hot parameter update plus a
// replacement land mid-stream (the TSan target for P5).
TEST(EngineTest, ThreadedEngineRunsPluginAsyncOperator) {
  ge::EngineConfig c = InlineConfig();
  c.cpu_threads = 2;
  c.watchdog_thread = true;
  c.watchdog_period = std::chrono::milliseconds(1);
  auto engine = ge::Engine::Create(c);
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;
  ASSERT_TRUE(e.LoadPlugin(PluginDir() / "sample_plugin.json").ok());
  ASSERT_TRUE(e.LoadPlugin(PluginDir() / "sample_plugin_v2.json").ok());
  auto s = e.CreateSession(Linear("t", 3000, "AsyncPass@1.0.0"));
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ASSERT_TRUE((*s)->Start().ok());
  ASSERT_TRUE((*s)->SetParameters("pass", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(3)}})).ok());
  auto rep = e.UpgradeOperator(Op("AsyncPass@1.0.0"), Op("AsyncPass@2.0.0"));
  ASSERT_TRUE(rep.ok()) << rep.status().ToString();
  EXPECT_TRUE(rep->all_succeeded());
  ASSERT_TRUE((*s)->WaitStopped(std::chrono::seconds(30)));
  const ge::JsonValue snap = (*s)->Snapshot();
  EXPECT_EQ(Metric(snap, "sink", "packets_in"), 3000);
  EXPECT_EQ(NodeOp(snap, "pass"), "AsyncPass@2.0.0");
  EXPECT_EQ(AsyncMetric(snap, "pass", "in_flight"), 0);
  EXPECT_EQ(AsyncMetric(snap, "pass", "reorder_gaps"), 0);
  EXPECT_EQ(e.async_runtime().in_flight(), 0U);
  EXPECT_EQ(e.async_runtime().orphan_completion_total(), 0U);
  EXPECT_EQ(e.async_runtime().timeout_gap_total(), 0U);
}

// ---------------------------------------------------------------------------
// C API (13 §5): the same scenarios through ge_engine_* / ge_session_*.
// ---------------------------------------------------------------------------

struct CEngine {
  ge_engine_handle h = nullptr;
  std::string paths;
  CEngine() {
    paths = std::string("[\"") + PluginDir().string() + "\"]";
    ge_engine_config cfg;
    cfg.header = GE_STRUCT_HEADER_INIT(ge_engine_config);
    cfg.plugin_search_paths_json = paths.c_str();
    cfg.resource_limits_json = R"({"cpu_threads":0})";
    cfg.observability_config_json = R"({"watchdog_period_ms":1})";
    const ge_status st = ge_engine_create(&cfg, &h);
    EXPECT_EQ(st.code, GE_STATUS_OK) << (st.message ? st.message : "");
  }
  ~CEngine() { ge_engine_destroy(h); }
  ge::Engine& cpp() { return *reinterpret_cast<std::unique_ptr<ge::Engine>*>(h)->get(); }
};

std::string GraphJson(const char* name, int count, const char* pass = "Pass@1.0.0") {
  return ge::GraphSpecParser::ToJson(Linear(name, count, pass)).Serialize();
}

std::string TakeString(ge_engine_handle e, char* s) {
  std::string out = s != nullptr ? s : "";
  ge_string_free(e, s);
  return out;
}

TEST(CApiTest, EngineCreateRejectsBadHeaderAndConfig) {
  ge_engine_handle h = nullptr;
  ge_engine_config cfg;
  cfg.header = GE_STRUCT_HEADER_INIT(ge_engine_config);
  cfg.plugin_search_paths_json = nullptr;
  cfg.resource_limits_json = nullptr;
  cfg.observability_config_json = nullptr;
  EXPECT_EQ(ge_engine_create(nullptr, &h).code, GE_STATUS_PLUGIN_ABI_MISMATCH);
  EXPECT_EQ(ge_engine_create(&cfg, nullptr).code, GE_STATUS_INVALID_ARGUMENT);
  cfg.header.abi_major = GE_ABI_MAJOR + 1;
  EXPECT_EQ(ge_engine_create(&cfg, &h).code, GE_STATUS_PLUGIN_ABI_MISMATCH);
  EXPECT_EQ(h, nullptr);
  cfg.header = GE_STRUCT_HEADER_INIT(ge_engine_config);
  cfg.resource_limits_json = "{bad";
  const ge_status st = ge_engine_create(&cfg, &h);
  EXPECT_EQ(st.code, GE_STATUS_INVALID_ARGUMENT);
  EXPECT_NE(st.message, nullptr);
  cfg.resource_limits_json = nullptr;
  ASSERT_EQ(ge_engine_create(&cfg, &h).code, GE_STATUS_OK);
  ASSERT_NE(h, nullptr);
  ge_engine_destroy(h);
  ge_engine_destroy(nullptr);
  // Null-handle guards never crash.
  ge_plugin_id pid = 0;
  EXPECT_EQ(ge_engine_load_plugin(nullptr, "x", &pid).code, GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(ge_session_start(nullptr).code, GE_STATUS_INVALID_ARGUMENT);
  ge_session_destroy(nullptr);
  ge_subscription_cancel(nullptr);
  ge_string_free(nullptr, nullptr);
}

TEST(CApiTest, PluginLoadRejectionsAndCapability) {
  CEngine e;
  ge_plugin_id v1 = 0;
  const std::string good = (PluginDir() / "sample_plugin.json").string();
  ASSERT_EQ(ge_engine_load_plugin(e.h, good.c_str(), &v1).code, GE_STATUS_OK);
  EXPECT_NE(v1, 0U);
  ge_plugin_id bad = 0;
  const std::string abi = (PluginDir() / "bad_abi_major.json").string();
  ge_status st = ge_engine_load_plugin(e.h, abi.c_str(), &bad);
  EXPECT_EQ(st.code, GE_STATUS_PLUGIN_ABI_MISMATCH);
  ASSERT_NE(st.detail_json, nullptr);
  EXPECT_NE(std::string(st.detail_json).find("\"stage\""), std::string::npos);
  const std::string dep = (PluginDir() / "bad_dependency.json").string();
  EXPECT_EQ(ge_engine_load_plugin(e.h, dep.c_str(), &bad).code, GE_STATUS_PLUGIN_MANIFEST_INVALID);
  const std::string missing = (PluginDir() / "nope.json").string();
  EXPECT_EQ(ge_engine_load_plugin(e.h, missing.c_str(), &bad).code, GE_STATUS_PLUGIN_MANIFEST_INVALID);
  // Loading twice reports ALREADY_EXISTS, the first stays registered.
  EXPECT_EQ(ge_engine_load_plugin(e.h, good.c_str(), &bad).code, GE_STATUS_ALREADY_EXISTS);
  EXPECT_EQ(e.cpp().plugins().Get(v1)->state, ge::PluginState::kRegistered);
  char* json = nullptr;
  ASSERT_EQ(ge_engine_get_capability_json(e.h, "Pass@1.0.0", &json).code, GE_STATUS_OK);
  const std::string cap = TakeString(e.h, json);
  EXPECT_NE(cap.find("\"hot_updatable\""), std::string::npos);
  EXPECT_EQ(ge_engine_get_capability_json(e.h, "Pass@9.0.0", &json).code, GE_STATUS_NOT_FOUND);
  EXPECT_EQ(ge_engine_get_capability_json(e.h, "garbage", &json).code, GE_STATUS_INVALID_ARGUMENT);
}

struct EventSink {
  std::mutex mu;
  std::vector<std::string> types;
  std::vector<std::string> details;
  std::atomic<int> count{0};
  static void Callback(const ge_event* ev, void* user) {
    auto* self = static_cast<EventSink*>(user);
    std::lock_guard lock(self->mu);
    self->types.emplace_back(ev->type);
    self->details.emplace_back(ev->detail_json != nullptr ? ev->detail_json : "");
    self->count.fetch_add(1);
  }
};

TEST(CApiTest, SessionLifecycleSnapshotEventsAndParameters) {
  CEngine e;
  ge_plugin_id v1 = 0;
  const std::string good = (PluginDir() / "sample_plugin.json").string();
  ASSERT_EQ(ge_engine_load_plugin(e.h, good.c_str(), &v1).code, GE_STATUS_OK);
  const std::string graph = GraphJson("c", 60);
  ge_session_handle s = nullptr;
  ge_operation_id create_op = 0;
  ge_status st = ge_session_create(e.h, graph.c_str(), R"({"caller_id":"t","request_id":"r1"})", &s, &create_op);
  ASSERT_EQ(st.code, GE_STATUS_OK) << (st.message ? st.message : "");
  ASSERT_NE(s, nullptr);
  char* json = nullptr;
  ASSERT_EQ(ge_engine_get_operation_json(e.h, create_op, &json).code, GE_STATUS_OK);
  std::string op_json = TakeString(e.h, json);
  EXPECT_NE(op_json.find("\"succeeded\""), std::string::npos);
  EXPECT_NE(op_json.find("\"r1\""), std::string::npos);
  EXPECT_EQ(ge_engine_get_operation_json(e.h, 99999, &json).code, GE_STATUS_NOT_FOUND);

  EventSink sink;
  ge_event_filter filter;
  filter.header = GE_STRUCT_HEADER_INIT(ge_event_filter);
  filter.filter_json = R"({"types":["session_state","sample.packet","log"]})";
  ge_subscription_handle sub = nullptr;
  ASSERT_EQ(ge_session_subscribe_events(s, &filter, &EventSink::Callback, &sink, &sub).code, GE_STATUS_OK);
  ge_event_filter bad_filter = filter;
  bad_filter.header.abi_major = GE_ABI_MAJOR + 1;
  ge_subscription_handle bad_sub = nullptr;
  EXPECT_EQ(ge_session_subscribe_events(s, &bad_filter, &EventSink::Callback, &sink, &bad_sub).code,
            GE_STATUS_PLUGIN_ABI_MISMATCH);

  ASSERT_EQ(ge_session_start(s).code, GE_STATUS_OK);
  EXPECT_EQ(ge_session_start(s).code, GE_STATUS_INVALID_ARGUMENT);  // already running
  ge::Session* cpp = e.cpp().FindSession(1);
  ASSERT_NE(cpp, nullptr);
  RunSome(e.cpp(), cpp, 5);
  ASSERT_EQ(ge_session_pause(s).code, GE_STATUS_OK);
  EXPECT_EQ(cpp->state(), ge::SessionState::kPaused);
  // Hot parameter through the C API; the sink observes the new gain byte.
  ge_parameter_version pv = 0;
  ge_operation_id pop = 0;
  const ge_node_id pass_id = cpp->current_topology()->FindNode("pass")->id();
  ASSERT_EQ(ge_session_set_node_parameters(s, pass_id, R"({"gain":7})", nullptr, &pv, &pop).code, GE_STATUS_OK);
  EXPECT_GE(pv, 1U);
  EXPECT_EQ(ge_session_set_node_parameters(s, pass_id, R"({"nope":1})", nullptr, &pv, &pop).code,
            GE_STATUS_PARAMETER_UNSUPPORTED);
  EXPECT_EQ(ge_session_set_node_parameters(s, 4242, R"({"gain":1})", nullptr, &pv, &pop).code, GE_STATUS_NOT_FOUND);
  EXPECT_EQ(ge_session_set_node_parameters(s, pass_id, "{bad", nullptr, &pv, &pop).code, GE_STATUS_INVALID_ARGUMENT);
  ASSERT_EQ(ge_session_resume(s).code, GE_STATUS_OK);
  ASSERT_TRUE(RunToStop(e.cpp(), cpp));

  ASSERT_EQ(ge_session_get_snapshot_json(s, &json).code, GE_STATUS_OK);
  const std::string snap = TakeString(e.h, json);
  auto parsed = ge::ParseJson(snap);
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(parsed.value->GetString("state").value_or(""), "stopped");
  EXPECT_EQ(Metric(*parsed.value, "sink", "packets_in"), 60);
  EXPECT_EQ(Metric(*parsed.value, "src", "packets_out"), 60);

  e.cpp().events().Flush();
  {
    std::lock_guard lock(sink.mu);
    bool saw_gain = false;
    bool saw_state = false;
    bool saw_log = false;
    for (std::size_t i = 0; i < sink.types.size(); ++i) {
      if (sink.types[i] == "sample.packet" && sink.details[i].find("\"gain\":7") != std::string::npos) saw_gain = true;
      if (sink.types[i] == "session_state") saw_state = true;
      if (sink.types[i] == "log" && sink.details[i].find("sink flushed") != std::string::npos) saw_log = true;
    }
    EXPECT_TRUE(saw_gain);
    EXPECT_TRUE(saw_state);
    EXPECT_TRUE(saw_log);
  }
  ge_subscription_cancel(sub);
  // Stop on a stopped session is fine (idempotent operation).
  ge_operation_id stop_op = 0;
  EXPECT_EQ(ge_session_stop(s, 1, &stop_op).code, GE_STATUS_OK);
  ge_session_destroy(s);
  EXPECT_EQ(e.cpp().Sessions().size(), 0U);
  EXPECT_EQ(e.cpp().plugins().Get(v1)->references, 0U);
}

TEST(CApiTest, SessionCreateFailuresAndDestroyedHandles) {
  CEngine e;
  ge_plugin_id v1 = 0;
  const std::string good = (PluginDir() / "sample_plugin.json").string();
  ASSERT_EQ(ge_engine_load_plugin(e.h, good.c_str(), &v1).code, GE_STATUS_OK);
  ge_session_handle s = nullptr;
  EXPECT_EQ(ge_session_create(e.h, "{bad", nullptr, &s, nullptr).code, GE_STATUS_GRAPH_INVALID);
  EXPECT_EQ(s, nullptr);
  EXPECT_EQ(ge_session_create(e.h, GraphJson("x", 1).c_str(), "[1]", &s, nullptr).code, GE_STATUS_INVALID_ARGUMENT);
  ge_operation_id op = 0;
  const ge_status unknown = ge_session_create(e.h, GraphJson("x", 1, "Nope@1.0.0").c_str(), nullptr, &s, &op);
  EXPECT_EQ(unknown.code, GE_STATUS_NOT_FOUND);
  EXPECT_NE(op, 0U);  // the failed create is still an operation record
  char* json = nullptr;
  ASSERT_EQ(ge_engine_get_operation_json(e.h, op, &json).code, GE_STATUS_OK);
  EXPECT_NE(TakeString(e.h, json).find("\"failed\""), std::string::npos);
  // Warm-up failure through the C API.
  ge::GraphBuilder g("open");
  auto src = g.AddNode(Op("Src@1.0.0"), "src");
  auto snk = g.AddNode(Op("Sink@1.0.0"), "sink", ge::JsonValue(ge::JsonObject{{"fail_open", ge::JsonValue(1)}}));
  g.Connect(src.port("out"), snk.port("in"));
  const std::string bad_open = ge::GraphSpecParser::ToJson(*g.Build()).Serialize();
  ASSERT_EQ(ge_session_create(e.h, bad_open.c_str(), nullptr, &s, nullptr).code, GE_STATUS_OK);
  const ge_status st = ge_session_start(s);
  EXPECT_EQ(st.code, GE_STATUS_NODE_WARMUP_FAILED);
  EXPECT_NE(std::string(st.message).find("fail_open"), std::string::npos);
  ge_session_destroy(s);
  EXPECT_EQ(e.cpp().plugins().Get(v1)->references, 0U);
  // Apply/dry-run patch through the C API.
  ASSERT_EQ(ge_session_create(e.h, GraphJson("p", 500).c_str(), nullptr, &s, nullptr).code, GE_STATUS_OK);
  ASSERT_EQ(ge_session_start(s).code, GE_STATUS_OK);
  ge::Session* cpp = e.cpp().FindSession(3);
  ASSERT_NE(cpp, nullptr);
  RunSome(e.cpp(), cpp, 5);
  const std::string patch =
      ge::GraphSpecParser::ToJson(ge::Mutation().SetNodeOptions("pass", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(3)}})).Build())
          .Serialize();
  ASSERT_EQ(ge_session_dry_run_patch(s, patch.c_str(), &json).code, GE_STATUS_OK);
  TakeString(e.h, json);
  EXPECT_EQ(ge_session_dry_run_patch(s, "{bad", &json).code, GE_STATUS_GRAPH_INVALID);
  ASSERT_EQ(ge_session_apply_patch(s, patch.c_str(), nullptr, &op).code, GE_STATUS_OK);
  ASSERT_TRUE(RunToStop(e.cpp(), cpp));
  ASSERT_EQ(ge_engine_get_operation_json(e.h, op, &json).code, GE_STATUS_OK);
  EXPECT_NE(TakeString(e.h, json).find("\"succeeded\""), std::string::npos);
  ge_session_destroy(s);
}

TEST(CApiTest, RetireAndUpgradeThroughCApi) {
  CEngine e;
  ge_plugin_id v1 = 0;
  ge_plugin_id v2 = 0;
  const std::string p1 = (PluginDir() / "sample_plugin.json").string();
  const std::string p2 = (PluginDir() / "sample_plugin_v2.json").string();
  ASSERT_EQ(ge_engine_load_plugin(e.h, p1.c_str(), &v1).code, GE_STATUS_OK);
  ASSERT_EQ(ge_engine_load_plugin(e.h, p2.c_str(), &v2).code, GE_STATUS_OK);
  ge_session_handle a = nullptr;
  ge_session_handle b = nullptr;
  ge_session_handle c = nullptr;
  ASSERT_EQ(ge_session_create(e.h, GraphJson("a", 300).c_str(), nullptr, &a, nullptr).code, GE_STATUS_OK);
  ASSERT_EQ(ge_session_create(e.h, GraphJson("b", 300).c_str(), nullptr, &b, nullptr).code, GE_STATUS_OK);
  ASSERT_EQ(ge_session_create(e.h, GraphJson("c", 300, "Pass@2.0.0").c_str(), nullptr, &c, nullptr).code, GE_STATUS_OK);
  ASSERT_EQ(ge_session_start(a).code, GE_STATUS_OK);
  ASSERT_EQ(ge_session_start(b).code, GE_STATUS_OK);
  ASSERT_EQ(ge_session_start(c).code, GE_STATUS_OK);
  ge::Session* ca = e.cpp().FindSession(1);
  ge::Session* cb = e.cpp().FindSession(2);
  ge::Session* cc = e.cpp().FindSession(3);
  RunSome(e.cpp(), ca, 5);
  RunSome(e.cpp(), cb, 5);
  RunSome(e.cpp(), cc, 5);
  ge_operation_id op = 0;
  EXPECT_EQ(ge_engine_upgrade_operator(e.h, "Pass@1.0.0", "bad key", &op).code, GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(ge_engine_upgrade_operator(e.h, "Nope@1.0.0", "Pass@2.0.0", &op).code, GE_STATUS_NOT_FOUND);
  ge_status st = ge_engine_upgrade_operator(e.h, "Pass@1.0.0", "Pass@2.0.0", &op);
  EXPECT_EQ(st.code, GE_STATUS_OK) << (st.message ? st.message : "") << (st.detail_json ? st.detail_json : "");
  EXPECT_NE(op, 0U);
  char* json = nullptr;
  ASSERT_EQ(ge_engine_get_operation_json(e.h, op, &json).code, GE_STATUS_OK);
  const std::string rep = TakeString(e.h, json);
  EXPECT_NE(rep.find("\"all_succeeded\":true"), std::string::npos);
  EXPECT_NE(rep.find("\"succeeded\""), std::string::npos);
  // Old version is retiring: no new sessions can use it, running ones finish.
  ge_session_handle late = nullptr;
  EXPECT_EQ(ge_session_create(e.h, GraphJson("late", 3).c_str(), nullptr, &late, nullptr).code, GE_STATUS_PLUGIN_RETIRED);
  ASSERT_TRUE(RunToStop(e.cpp(), ca));
  ASSERT_TRUE(RunToStop(e.cpp(), cb));
  ASSERT_TRUE(RunToStop(e.cpp(), cc));
  for (ge_session_handle h : {a, b, c}) {
    ASSERT_EQ(ge_session_get_snapshot_json(h, &json).code, GE_STATUS_OK);
    auto snap = ge::ParseJson(TakeString(e.h, json));
    ASSERT_TRUE(snap.ok());
    EXPECT_EQ(Metric(*snap.value, "sink", "packets_in"), 300);
    EXPECT_EQ(NodeOp(*snap.value, "pass"), "Pass@2.0.0");
  }
  // Retire v2 with physical unload: stays running while sessions reference it.
  ge_operation_id rop = 0;
  ASSERT_EQ(ge_engine_retire_plugin(e.h, v2, 1, &rop).code, GE_STATUS_OK);
  ASSERT_EQ(ge_engine_get_operation_json(e.h, rop, &json).code, GE_STATUS_OK);
  EXPECT_NE(TakeString(e.h, json).find("\"running\""), std::string::npos);
  EXPECT_EQ(ge_engine_retire_plugin(e.h, 777, 0, &rop).code, GE_STATUS_NOT_FOUND);
  ge_session_destroy(a);
  ge_session_destroy(b);
  // c still references v1 (src/sink) and v2 (pass): both keep their state.
  EXPECT_EQ(e.cpp().plugins().Get(v1)->state, ge::PluginState::kRetiring);
  EXPECT_EQ(e.cpp().plugins().Get(v2)->state, ge::PluginState::kRetiring);
  ge_session_destroy(c);
  ASSERT_EQ(ge_engine_get_operation_json(e.h, rop, &json).code, GE_STATUS_OK);
  const std::string done = TakeString(e.h, json);
  EXPECT_NE(done.find("\"succeeded\""), std::string::npos);
  EXPECT_NE(done.find("\"physically_unloaded\":true"), std::string::npos);
  EXPECT_EQ(e.cpp().plugins().Get(v2)->state, ge::PluginState::kPhysicallyUnloaded);
  EXPECT_EQ(e.cpp().plugins().Get(v1)->state, ge::PluginState::kLogicallyUnloaded);
  // Both ids are free again.
  ASSERT_EQ(ge_engine_load_plugin(e.h, p2.c_str(), &v2).code, GE_STATUS_OK);
}

}  // namespace
