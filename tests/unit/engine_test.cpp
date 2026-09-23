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

// Defined in src/c_api.cpp; not exported, not in the public headers. Lets the
// C API tests drive the C++ side of a session the C API created.
ge::Engine* ge_engine_for_testing(ge_engine_handle engine);

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
  // TD-03 resource limits: kind[@device] -> capacity.
  auto r = ge::EngineConfig::FromJson(
      nullptr,
      R"({"cpu_threads":2,"resources":{"cpu_threads":8,"host_memory_bytes":4096,"gpu_memory@1":99},"reject_unbudgeted_edges":true})",
      nullptr);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  ASSERT_EQ(r->resource_capacities.size(), 3U);  // object keys are sorted
  EXPECT_EQ(r->resource_capacities[0].kind, ge::ResourceKind::kCpuThreads);
  EXPECT_EQ(r->resource_capacities[0].capacity, 8U);
  EXPECT_EQ(r->resource_capacities[1].kind, ge::ResourceKind::kGpuMemory);
  EXPECT_EQ(r->resource_capacities[1].device_id, 1);
  EXPECT_EQ(r->resource_capacities[2].kind, ge::ResourceKind::kHostMemory);
  EXPECT_TRUE(r->reject_unbudgeted_edges);
  EXPECT_FALSE(ge::EngineConfig::FromJson(nullptr, R"({"resources":{"unicorns":1}})", nullptr).ok());
  EXPECT_FALSE(ge::EngineConfig::FromJson(nullptr, R"({"resources":{"gpu_memory":1}})", nullptr).ok());
  EXPECT_FALSE(ge::EngineConfig::FromJson(nullptr, R"({"resources":{"cpu_threads":-1}})", nullptr).ok());
  auto off = ge::EngineConfig::FromJson(nullptr, R"({"admission":false})", nullptr);
  ASSERT_TRUE(off.ok());
  EXPECT_FALSE(off->resource_admission);
  // A search path that is not a directory is refused at create.
  ge::EngineConfig bad = InlineConfig();
  bad.plugin_search_paths.emplace_back(PluginDir() / "does_not_exist");
  EXPECT_EQ(ge::Engine::Create(bad).status().code(), GE_STATUS_INVALID_ARGUMENT);
}

TEST(EngineTest, LedgerDefaultsFollowConfigAndSessionsReserve) {
  ge::EngineConfig c = InlineConfig();
  c.cpu_threads = 0;
  c.resource_capacities = {{ge::ResourceKind::kCpuThreads, -1, 3}};
  auto e = ge::Engine::Create(c);
  ASSERT_TRUE(e.ok()) << e.status().ToString();
  ge::ResourceLedger* ledger = (*e)->resource_ledger();
  ASSERT_NE(ledger, nullptr);
  EXPECT_EQ(ledger->Capacity(ge::ResourceKind::kCpuThreads, -1), 3U);
  EXPECT_TRUE(ledger->Capacity(ge::ResourceKind::kHostMemory, -1).has_value());  // platform default
  ASSERT_TRUE((*e)->LoadPlugin(PluginDir() / "sample_plugin.json").ok());
  // Sample plugin operators declare no resources: the session holds a lease
  // with nothing in it.
  auto s = (*e)->CreateSession(Linear("g", 5));
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  EXPECT_EQ(ledger->live_leases(), 1U);
  EXPECT_TRUE((*s)->HeldResources().empty());
  ASSERT_TRUE((*e)->DestroySession((*s)->id()).ok());
  EXPECT_EQ(ledger->live_leases(), 0U);

  ge::EngineConfig off = InlineConfig();
  off.resource_admission = false;
  auto e2 = ge::Engine::Create(off);
  ASSERT_TRUE(e2.ok());
  EXPECT_EQ((*e2)->resource_ledger(), nullptr);
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
  // OBS-1: ingress stamps survive the async hop, so every packet the sink
  // consumed produced an end-to-end sample; async_wait sampled per request.
  const ge::SessionMetrics& sm = (*s)->scheduler().metrics();
  EXPECT_EQ(sm.end_to_end.count(), 300U);
  EXPECT_GT(sm.end_to_end.sum_ns(), 0U);
  const std::string prom = e.RenderPrometheus();
  EXPECT_NE(prom.find("ge_node_async_wait_seconds_count{session=\"1\",node=\"pass\",op=\"AsyncPass@1.0.0\"} 300\n"),
            std::string::npos)
      << prom;
  EXPECT_NE(prom.find("ge_node_async_completed_total{session=\"1\",node=\"pass\",op=\"AsyncPass@1.0.0\"} 300\n"),
            std::string::npos);
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

// docs/09 "so 升级后 24h 无延迟崩溃", compressed: upgrade the operator,
// physically unload (dlclose) the old plugin once its references drop, then
// keep running sessions, mutations and snapshots on the survivor. A dangling
// reference into the unloaded so (descriptor, vtable, string) crashes here,
// not 24h later. Also covers "加载卸载零数据面影响": the running stream's
// packet count is exact throughout.
TEST(EngineTest, PhysicalUnloadAfterUpgradeLeavesNoDanglingReferences) {
  auto engine = ge::Engine::Create(InlineConfig());
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;
  auto v1 = e.LoadPlugin(PluginDir() / "sample_plugin.json");
  auto v2 = e.LoadPlugin(PluginDir() / "sample_plugin_v2.json");
  ASSERT_TRUE(v1.ok() && v2.ok());
  auto s = e.CreateSession(Linear("live", 500));
  ASSERT_TRUE(s.ok());
  ASSERT_TRUE((*s)->Start().ok());
  RunSome(e, *s, 10);

  auto rep = e.UpgradeOperator(Op("Pass@1.0.0"), Op("Pass@2.0.0"));
  ASSERT_TRUE(rep.ok()) << rep.status().ToString();
  EXPECT_TRUE(rep->all_succeeded());
  // v1 is retiring but still referenced (src/sink); ask for a physical
  // unload, which must wait for those references instead of dlclosing a
  // library the running graph still executes.
  auto retire = e.RetirePlugin(v1->id, {.request_physical_unload = true});
  ASSERT_TRUE(retire.ok());
  EXPECT_EQ(e.plugins().Get(v1->id)->state, ge::PluginState::kRetiring);
  ASSERT_TRUE(RunToStop(e, *s));
  EXPECT_EQ(Metric((*s)->Snapshot(), "sink", "packets_in"), 500);  // zero data-plane impact
  ASSERT_TRUE(e.DestroySession((*s)->id()).ok());
  e.Tick();  // references dropped -> pending physical unload completes
  EXPECT_EQ(e.plugins().Get(v1->id)->state, ge::PluginState::kPhysicallyUnloaded);
  EXPECT_EQ(e.operations().Get(*retire)->detail.GetBool("physically_unloaded").value_or(false), true);

  // Life after dlclose: everything that could hold a pointer into the old
  // so still works -- new sessions on v2 operators only, hot updates,
  // snapshots, capability queries, audit, and repeated full streams.
  EXPECT_EQ(e.GetCapability(Op("Pass@1.0.0")).status().code(), GE_STATUS_NOT_FOUND);
  auto cap = e.GetCapability(Op("Pass@2.0.0"));
  ASSERT_TRUE(cap.ok());
  const auto v2_linear = [](const char* name, int count) {
    ge::GraphBuilder g(name);
    auto src = g.AddNode(Op("Src@2.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(count)}}));
    auto pass = g.AddNode(Op("Pass@2.0.0"), "pass");
    auto sink = g.AddNode(Op("Sink@2.0.0"), "sink");
    g.Connect(src.port("out"), pass.port("in"), {.id = "e0"});
    g.Connect(pass.port("out"), sink.port("in"), {.id = "e1"});
    return *g.Build();
  };
  for (int round = 0; round < 3; ++round) {
    auto s2 = e.CreateSession(v2_linear(("after" + std::to_string(round)).c_str(), 200));
    ASSERT_TRUE(s2.ok()) << s2.status().ToString();
    ASSERT_TRUE((*s2)->Start().ok());
    RunSome(e, *s2, 5);
    ASSERT_TRUE((*s2)->SetParameters("pass", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(round)}})).ok());
    ASSERT_TRUE(RunToStop(e, *s2));
    EXPECT_EQ(Metric((*s2)->Snapshot(), "sink", "packets_in"), 200);
    EXPECT_EQ(NodeOp((*s2)->Snapshot(), "pass"), "Pass@2.0.0");
    ASSERT_TRUE(e.DestroySession((*s2)->id()).ok());
  }
  // The audit trail of the unloaded plugin survives the dlclose (PLG-8).
  bool unload_audited = false;
  for (const ge::PluginAuditRecord& r : e.plugins().Audit()) {
    unload_audited = unload_audited || (r.plugin_id == v1->id && r.action == "physical_unload");
  }
  EXPECT_TRUE(unload_audited);
}

// ---------------------------------------------------------------------------
// GM-3 / TD-01: template reuse (A4/A5 once per template) vs. operator set
// changes.
// ---------------------------------------------------------------------------

// The prevalidated cache must not outlive the operator set it was computed
// against: after a plugin load the instance is revalidated instead of being
// negotiated against capabilities that no longer describe the registry.
TEST(EngineTest, PrevalidatedTemplateIsRevalidatedAfterOperatorSetChange) {
  auto engine = ge::Engine::Create(InlineConfig());
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;
  ASSERT_TRUE(e.LoadPlugin(PluginDir() / "sample_plugin.json").ok());

  auto created = ge::GraphTemplate::Create(Linear("t", 5), {});
  ASSERT_TRUE(created.ok()) << created.status().ToString();
  ge::GraphTemplate tmpl = std::move(*created);
  ASSERT_TRUE(e.PrevalidateTemplate(tmpl).ok());
  const std::uint64_t stamp = tmpl.validated_generation();
  EXPECT_EQ(stamp, e.operator_generation());
  EXPECT_TRUE(tmpl.validated_for(e.operator_generation()));

  // A second plugin moves the operator generation, so the stamp is stale...
  ASSERT_TRUE(e.LoadPlugin(PluginDir() / "sample_plugin_v2.json").ok());
  EXPECT_GT(e.operator_generation(), stamp);
  EXPECT_FALSE(tmpl.validated_for(e.operator_generation()));
  // ...but creating an instance still works: it revalidates internally
  // instead of using the cache.
  auto s = e.CreateSession(tmpl, ge::JsonValue(ge::JsonObject{}));
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ASSERT_TRUE((*s)->Start().ok());
  ASSERT_TRUE(RunToStop(e, *s));
  EXPECT_EQ(Metric((*s)->Snapshot(), "sink", "packets_in"), 5);
}

TEST(EngineTest, PrevalidateTemplateRevalidatesOnSecondCall) {
  auto engine = ge::Engine::Create(InlineConfig());
  ASSERT_TRUE(engine.ok());
  ge::Engine& e = **engine;
  auto created = ge::GraphTemplate::Create(Linear("t", 5), {});
  ASSERT_TRUE(created.ok());
  ge::GraphTemplate tmpl = std::move(*created);
  // Unknown operators: prevalidation fails and caches nothing.
  EXPECT_FALSE(e.PrevalidateTemplate(tmpl).ok());
  EXPECT_EQ(tmpl.validated(), nullptr);
  EXPECT_FALSE(tmpl.validated_for(e.operator_generation()));

  ASSERT_TRUE(e.LoadPlugin(PluginDir() / "sample_plugin.json").ok());
  ASSERT_TRUE(e.PrevalidateTemplate(tmpl).ok());
  EXPECT_TRUE(tmpl.validated_for(e.operator_generation()));
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
  ge::Engine& cpp() { return *ge_engine_for_testing(h); }
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

// Lifecycle (Code Complete -- Defensive Programming): a session/subscription
// handle whose engine is gone must resolve to a status, never dereference
// freed memory. Regression test for the dangling ge_engine_t* owner. Destroying
// the engine handle itself is not exercised here: the handle address is the
// host's to keep alive, so using it afterwards is out of contract (13 §3.2).
TEST(CApiTest, HandlesSurviveEngineDestroyWithoutDereferencingFreedMemory) {
  std::string paths = std::string("[\"") + PluginDir().string() + "\"]";
  ge_engine_config cfg;
  cfg.header = GE_STRUCT_HEADER_INIT(ge_engine_config);
  cfg.plugin_search_paths_json = paths.c_str();
  cfg.resource_limits_json = nullptr;
  cfg.observability_config_json = nullptr;
  ge_engine_handle h = nullptr;
  ASSERT_EQ(ge_engine_create(&cfg, &h).code, GE_STATUS_OK);
  ge_plugin_id pid = 0;
  const std::string manifest = (PluginDir() / "sample_plugin.json").string();
  ASSERT_EQ(ge_engine_load_plugin(h, manifest.c_str(), &pid).code, GE_STATUS_OK);

  ge_session_handle s = nullptr;
  ge_status cs = ge_session_create(h, GraphJson("h", 5).c_str(), nullptr, &s, nullptr);
  ASSERT_EQ(cs.code, GE_STATUS_OK) << (cs.message ? cs.message : "");
  ASSERT_EQ(ge_session_start(s).code, GE_STATUS_OK);
  ge_subscription_handle sub = nullptr;
  ASSERT_EQ(ge_session_subscribe_events(s, nullptr, [](const ge_event*, void*) {}, nullptr, &sub).code,
            GE_STATUS_OK);

  ge_engine_destroy(h);  // the host destroys the engine before its handles

  EXPECT_EQ(ge_session_start(s).code, GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(ge_session_pause(s).code, GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(ge_session_resume(s).code, GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(ge_session_stop(s, 1, nullptr).code, GE_STATUS_INVALID_ARGUMENT);
  char* json = nullptr;
  EXPECT_EQ(ge_session_get_snapshot_json(s, &json).code, GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(json, nullptr);
  EXPECT_EQ(ge_session_apply_patch(s, "{}", nullptr, nullptr).code, GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(ge_session_set_node_parameters(s, 1, "{}", nullptr, nullptr, nullptr).code,
            GE_STATUS_INVALID_ARGUMENT);

  // Destroying them afterwards is a no-op, not a second dereference.
  ge_session_destroy(s);
  ge_subscription_cancel(sub);
}

// Concurrency (00 REL: no delayed crash after teardown): C API calls racing
// a DestroySession on another thread must never dereference a freed Session.
// Resolve() hands out shared ownership, so a call that started before the
// destroy finishes on a live object and later calls see INVALID_ARGUMENT.
// The destroy runs through the C++ engine (as the watchdog or another host
// thread would); the C handle itself stays alive for the whole race, since
// its own destroy is the host's serialisation point (13 §3.2). Threaded
// engine + 1ms watchdog keeps watchdog Tick() snapshots in the race too.
TEST(CApiTest, SessionCallsRacingDestroyResolveOrFailCleanly) {
  std::string paths = std::string("[\"") + PluginDir().string() + "\"]";
  ge_engine_config cfg;
  cfg.header = GE_STRUCT_HEADER_INIT(ge_engine_config);
  cfg.plugin_search_paths_json = paths.c_str();
  cfg.resource_limits_json = R"({"cpu_threads":2})";
  cfg.observability_config_json = R"({"watchdog_period_ms":1})";
  ge_engine_handle h = nullptr;
  ASSERT_EQ(ge_engine_create(&cfg, &h).code, GE_STATUS_OK);
  ge_plugin_id pid = 0;
  const std::string manifest = (PluginDir() / "sample_plugin.json").string();
  ASSERT_EQ(ge_engine_load_plugin(h, manifest.c_str(), &pid).code, GE_STATUS_OK);
  ge::Engine& e = *ge_engine_for_testing(h);

  for (int round = 0; round < 5; ++round) {
    ge_session_handle s = nullptr;
    const std::string graph = GraphJson(("race" + std::to_string(round)).c_str(), 100000);
    ASSERT_EQ(ge_session_create(h, graph.c_str(), nullptr, &s, nullptr).code, GE_STATUS_OK);
    ASSERT_EQ(ge_session_start(s).code, GE_STATUS_OK);
    const std::vector<ge::SessionId> ids = e.Sessions();
    ASSERT_EQ(ids.size(), 1U);

    std::atomic<bool> stop{false};
    std::atomic<int> ok_calls{0}, gone_calls{0};
    std::thread caller([&] {
      while (!stop.load(std::memory_order_acquire)) {
        char* json = nullptr;
        const ge_status st = ge_session_get_snapshot_json(s, &json);
        // Either a live snapshot or "destroyed"; never a crash.
        if (st.code == GE_STATUS_OK) {
          EXPECT_NE(json, nullptr);
          ge_string_free(h, json);
          ++ok_calls;
        } else {
          EXPECT_EQ(st.code, GE_STATUS_INVALID_ARGUMENT) << (st.message ? st.message : "");
          ++gone_calls;
        }
        (void)ge_session_pause(s);
        (void)ge_session_resume(s);
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    ASSERT_TRUE(e.DestroySession(ids[0]).ok());  // concurrent with the caller thread
    // After the destroy returned, every further call resolves to "destroyed".
    char* json = nullptr;
    EXPECT_EQ(ge_session_get_snapshot_json(s, &json).code, GE_STATUS_INVALID_ARGUMENT);
    stop.store(true, std::memory_order_release);
    caller.join();
    EXPECT_GT(ok_calls.load() + gone_calls.load(), 0);
    ge_session_destroy(s);  // no-op destroy of an already-destroyed session
  }
  ge_engine_destroy(h);
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

// OBS-1/2: the Prometheus exposition covers engine, ledger, session, node
// and edge metrics; end-to-end latency is sampled at the sink and mutation
// counters/histograms move with a hot update and a patch.
TEST(CApiTest, RenderPrometheusExposesAllLevels) {
  CEngine e;
  ge_plugin_id v1 = 0;
  const std::string good = (PluginDir() / "sample_plugin.json").string();
  ASSERT_EQ(ge_engine_load_plugin(e.h, good.c_str(), &v1).code, GE_STATUS_OK);
  char* text = nullptr;
  EXPECT_EQ(ge_engine_render_prometheus(nullptr, &text).code, GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(ge_engine_render_prometheus(e.h, nullptr).code, GE_STATUS_INVALID_ARGUMENT);
  ASSERT_EQ(ge_engine_render_prometheus(e.h, &text).code, GE_STATUS_OK);
  std::string empty = TakeString(e.h, text);
  EXPECT_NE(empty.find("ge_engine_sessions 0\n"), std::string::npos);
  EXPECT_NE(empty.find("# TYPE ge_resource_capacity gauge\n"), std::string::npos);
  EXPECT_NE(empty.find("ge_resource_capacity{kind=\"cpu_threads\",device=\"-1\"}"), std::string::npos);
  EXPECT_NE(empty.find("ge_engine_audit_records_total 1\n"), std::string::npos);  // plugin.load

  ge_session_handle s = nullptr;
  ASSERT_EQ(ge_session_create(e.h, GraphJson("m", 30).c_str(), nullptr, &s, nullptr).code, GE_STATUS_OK);
  ASSERT_EQ(ge_session_start(s).code, GE_STATUS_OK);
  ge::Session* cpp = e.cpp().FindSession(1);
  ASSERT_NE(cpp, nullptr);
  RunSome(e.cpp(), cpp, 5);
  ge_parameter_version pv = 0;
  ASSERT_EQ(ge_session_set_node_parameters(s, cpp->current_topology()->FindNode("pass")->id(), R"({"gain":2})",
                                           nullptr, &pv, nullptr).code,
            GE_STATUS_OK);
  const std::string patch =
      ge::GraphSpecParser::ToJson(ge::Mutation().SetNodeOptions("pass", ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(3)}})).Build())
          .Serialize();
  ge_operation_id op = 0;
  ASSERT_EQ(ge_session_apply_patch(s, patch.c_str(), nullptr, &op).code, GE_STATUS_OK);
  ASSERT_TRUE(RunToStop(e.cpp(), cpp));

  ASSERT_EQ(ge_engine_render_prometheus(e.h, &text).code, GE_STATUS_OK);
  const std::string out = TakeString(e.h, text);
  auto has = [&](const char* needle) { return out.find(needle) != std::string::npos; };
  SCOPED_TRACE(out);
  EXPECT_TRUE(has("ge_engine_sessions 1\n"));
  EXPECT_TRUE(has("ge_engine_executor_threads 0\n"));
  EXPECT_TRUE(has("ge_session_state{session=\"1\"} 6\n"));  // kStopped
  EXPECT_TRUE(has("ge_session_parameter_updates_total{session=\"1\"} 1\n"));
  EXPECT_TRUE(has("ge_session_mutations_succeeded_total{session=\"1\"} 1\n"));
  EXPECT_TRUE(has("ge_session_mutations_failed_total{session=\"1\"} 0\n"));
  EXPECT_TRUE(has("ge_session_retired_topologies_total{session=\"1\"} 1\n"));
  EXPECT_TRUE(has("ge_session_mutation_publish_seconds_count{session=\"1\"} 1\n"));
  EXPECT_TRUE(has("ge_session_mutation_retire_seconds_count{session=\"1\"} 1\n"));
  EXPECT_TRUE(has("ge_session_end_to_end_seconds_count{session=\"1\"} 30\n")) << out;
  EXPECT_TRUE(has("ge_session_end_to_end_seconds_bucket{session=\"1\",le=\"+Inf\"} 30\n"));
  EXPECT_TRUE(has("ge_node_packets_in_total{session=\"1\",node=\"sink\",op=\"Sink@1.0.0\"} 30\n"));
  EXPECT_TRUE(has("ge_node_packets_out_total{session=\"1\",node=\"src\",op=\"Src@1.0.0\"} 30\n"));
  EXPECT_TRUE(has("ge_node_process_seconds_count{session=\"1\",node=\"pass\",op=\"Pass@1.0.0\"} "));
  EXPECT_TRUE(has("ge_edge_pushed_total{session=\"1\",edge=\"e1\"} 31\n"));  // 30 data + EOS
  EXPECT_TRUE(has("ge_edge_queue_depth{session=\"1\",edge=\"e0\"} 0\n"));
  EXPECT_TRUE(has("# TYPE ge_node_process_seconds histogram\n"));
  // Every metric name is declared exactly once.
  EXPECT_EQ(out.find("# TYPE ge_node_packets_in_total counter"), out.rfind("# TYPE ge_node_packets_in_total counter"));
  // C++ entry point is the same renderer.
  EXPECT_EQ(e.cpp().RenderPrometheus(), out);
  ge_session_destroy(s);
}

// AUD-1/2: control operations, plugin loads and node failures land in the
// audit ring with the caller context, redacted, queryable through the C API.
TEST(CApiTest, AuditRecordsOperationsPluginsAndNodeFailures) {
  CEngine e;
  ge_plugin_id v1 = 0;
  const std::string good = (PluginDir() / "sample_plugin.json").string();
  ASSERT_EQ(ge_engine_load_plugin(e.h, good.c_str(), &v1).code, GE_STATUS_OK);
  ge_plugin_id bad = 0;
  const std::string abi = (PluginDir() / "bad_abi_major.json").string();
  EXPECT_EQ(ge_engine_load_plugin(e.h, abi.c_str(), &bad).code, GE_STATUS_PLUGIN_ABI_MISMATCH);

  // A graph whose node options carry a secret and a URL with credentials.
  ge::GraphBuilder g("aud");
  auto src = g.AddNode(Op("Src@1.0.0"), "src",
                       ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(20)},
                                                    {"url", ge::JsonValue("rtmp://u:p@cdn/live?token=abc")},
                                                    {"api_key", ge::JsonValue("K-123")}}));
  auto pass = g.AddNode(Op("Pass@1.0.0"), "pass");
  auto snk = g.AddNode(Op("Sink@1.0.0"), "sink");
  g.Connect(src.port("out"), pass.port("in"), {.id = "e0"});
  g.Connect(pass.port("out"), snk.port("in"), {.id = "e1"});
  const std::string graph = ge::GraphSpecParser::ToJson(*g.Build()).Serialize();
  ge_session_handle s = nullptr;
  ge_operation_id create_op = 0;
  ASSERT_EQ(ge_session_create(e.h, graph.c_str(), R"({"caller_id":"ops","request_id":"req-1"})", &s, &create_op).code,
            GE_STATUS_OK);
  ASSERT_EQ(ge_session_start(s).code, GE_STATUS_OK);
  ge::Session* cpp = e.cpp().FindSession(1);
  ASSERT_NE(cpp, nullptr);
  RunSome(e.cpp(), cpp, 2);
  // A failing parameter update (unknown node) and a node failure (fail_at).
  ge_operation_id bad_op = 0;
  EXPECT_NE(ge_session_set_node_parameters(s, 999, R"({"gain":1})", R"({"caller_id":"ops","request_id":"req-2"})",
                                           nullptr, &bad_op).code,
            GE_STATUS_OK);
  ge_parameter_version pv = 0;
  ge_operation_id set_op = 0;
  ASSERT_EQ(ge_session_set_node_parameters(s, cpp->current_topology()->FindNode("pass")->id(), R"({"fail_at":10})",
                                           R"({"caller_id":"ops","request_id":"req-3"})", &pv, &set_op).code,
            GE_STATUS_OK);
  for (int i = 0; i < 2000 && cpp->state() != ge::SessionState::kFailed; ++i) RunSome(e.cpp(), cpp, 1);
  EXPECT_EQ(cpp->state(), ge::SessionState::kFailed);

  char* json = nullptr;
  ASSERT_EQ(ge_engine_query_audit_json(e.h, nullptr, &json).code, GE_STATUS_OK);
  const std::string all = TakeString(e.h, json);
  const auto parsed = ge::ParseJson(all);
  ASSERT_TRUE(parsed.ok());
  const auto& records = parsed.value->as_object().at("records").as_array();
  std::map<std::string, int> ops;
  for (const auto& r : records) ops[r.as_object().at("operation").as_string()]++;
  EXPECT_EQ(ops["plugin.load"], 2);
  EXPECT_EQ(ops["session.create"], 1);
  EXPECT_EQ(ops["parameter.set"], 1);
  EXPECT_EQ(ops["node.failed"], 1);
  // AUD-2: the create digest carries the graph with secret/URL redacted.
  EXPECT_EQ(all.find("K-123"), std::string::npos);
  EXPECT_EQ(all.find("token=abc"), std::string::npos);
  EXPECT_NE(all.find("rtmp://***@cdn/live"), std::string::npos);
  EXPECT_NE(all.find("\"api_key\":\"***\""), std::string::npos);
  // Caller context and target on the node failure.
  for (const auto& r : records) {
    const auto& o = r.as_object();
    if (o.at("operation").as_string() == "node.failed") {
      EXPECT_EQ(o.at("caller_id").as_string(), "ops");
      EXPECT_EQ(o.at("request_id").as_string(), "req-1");
      EXPECT_EQ(o.at("target").as_string(), "node:pass");
      EXPECT_EQ(o.at("session_id").as_integer(), 1);
      EXPECT_NE(o.at("result").as_string(), "OK");
    }
    if (o.at("operation").as_string() == "session.create") {
      EXPECT_EQ(o.at("operation_id").as_integer(), static_cast<std::int64_t>(create_op));
      EXPECT_EQ(o.at("topology_version").as_integer(), 1);
    }
  }
  // Filters through the C API: failures only, by request id, paging.
  ASSERT_EQ(ge_engine_query_audit_json(e.h, R"({"failures_only":true})", &json).code, GE_STATUS_OK);
  const auto failures = ge::ParseJson(TakeString(e.h, json));
  const auto& frec = failures.value->as_object().at("records").as_array();
  EXPECT_GE(frec.size(), 2U);  // bad plugin + node failure (+ rejected set)
  for (const auto& r : frec) EXPECT_NE(r.as_object().at("result").as_string(), "OK");
  ASSERT_EQ(ge_engine_query_audit_json(e.h, R"({"request_id":"req-3"})", &json).code, GE_STATUS_OK);
  EXPECT_EQ(ge::ParseJson(TakeString(e.h, json)).value->as_object().at("records").as_array().size(), 1U);
  ASSERT_EQ(ge_engine_query_audit_json(e.h, R"({"limit":1})", &json).code, GE_STATUS_OK);
  const auto page = ge::ParseJson(TakeString(e.h, json));
  EXPECT_EQ(page.value->as_object().at("records").as_array().size(), 1U);
  EXPECT_EQ(ge_engine_query_audit_json(e.h, "{bad", &json).code, GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(ge_engine_query_audit_json(e.h, R"({"limit":-1})", &json).code, GE_STATUS_INVALID_ARGUMENT);
  // Sink sees records live: destroying a failed session issues session.stop.
  std::vector<std::string> sunk;
  e.cpp().audit().SetSink([&](const ge::AuditRecord& r) { sunk.push_back(r.operation); });
  ge_session_destroy(s);
  EXPECT_EQ(sunk, std::vector<std::string>{"session.stop"});
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
