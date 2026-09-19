#include <ge/cpp/plugin_registry.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <ge/cpp/session.h>

namespace {

namespace fs = std::filesystem;

fs::path PluginDir() {
  const char* env = std::getenv("GE_PLUGIN_DIR");
  if (env != nullptr) return fs::path(env);
  return fs::path(GE_PLUGIN_DIR);
}

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

struct Fixture {
  fs::path dir = PluginDir();
  std::shared_ptr<ge::HostBufferPool> pool = ge::HostBufferPool::Create();
  ge::PluginRegistryOptions options;
  std::unique_ptr<ge::PluginRegistry> registry;
  std::vector<std::pair<ge::PluginState, ge::PluginState>> transitions;

  explicit Fixture(std::map<std::string, std::string> deps = {}) {
    options.search_paths = {dir};
    options.host_dependencies = std::move(deps);
    options.services.pool = pool;
    registry = std::make_unique<ge::PluginRegistry>(options);
    registry->on_state_changed = [this](const ge::PluginInfo&, ge::PluginState a, ge::PluginState b) {
      transitions.emplace_back(a, b);
    };
  }
  ge::Result<ge::PluginInfo> Load(const char* manifest) { return registry->Load(dir / manifest); }
};

// ---------------------------------------------------------------------------
// Validation chain (12 §9.1) -- every failure leaves the registry unchanged.
// ---------------------------------------------------------------------------

TEST(PluginRegistryTest, LoadsSampleAndRegistersOperators) {
  Fixture f;
  auto info = f.Load("sample_plugin.json");
  ASSERT_TRUE(info.ok()) << info.status().ToString();
  EXPECT_EQ(info->plugin_id, "com.example.sample");
  EXPECT_EQ(info->state, ge::PluginState::kRegistered);
  EXPECT_EQ(info->build_fingerprint, GE_BUILD_FINGERPRINT);
  ASSERT_EQ(info->operators.size(), 4U);
  EXPECT_EQ(f.registry->Resolve(Op("Src@1.0.0")), info->id);
  EXPECT_EQ(f.registry->Resolve(Op("Pass@1.0.0")), info->id);
  const ge::CapabilityDescriptor* cap = f.registry->Describe(Op("Pass@1.0.0"));
  ASSERT_NE(cap, nullptr);
  EXPECT_EQ(cap->parameters.hot_updatable, (std::vector<std::string>{"gain", "fail_at"}));
  EXPECT_EQ(cap->parameters.migratable, std::vector<std::string>{"gain"});
  EXPECT_EQ(cap->description, "pass-through with gain");
  EXPECT_EQ(f.registry->Describe(Op("Nope@1.0.0")), nullptr);
  const auto audit = f.registry->Audit();
  ASSERT_FALSE(audit.empty());
  EXPECT_EQ(audit.back().action, "load");
}

TEST(PluginRegistryTest, RejectionsDoNotTouchRegistry) {
  Fixture f;
  ASSERT_TRUE(f.Load("sample_plugin.json").ok());
  const auto before = f.registry->List();
  struct Case {
    const char* manifest;
    ge_status_code code;
    const char* stage;
  };
  const Case cases[] = {
      {"bad_abi_major.json", GE_STATUS_PLUGIN_ABI_MISMATCH, "abi"},
      {"bad_header.json", GE_STATUS_PLUGIN_ABI_MISMATCH, "abi"},
      {"bad_operators.json", GE_STATUS_PLUGIN_MANIFEST_INVALID, "descriptor"},
      {"bad_vtable.json", GE_STATUS_PLUGIN_ABI_MISMATCH, "vtable"},
      {"bad_fingerprint.json", GE_STATUS_PLUGIN_ABI_MISMATCH, "fingerprint"},
      {"no_symbol.json", GE_STATUS_PLUGIN_ABI_MISMATCH, "symbols"},
      {"bad_parallelism.json", GE_STATUS_PLUGIN_MANIFEST_INVALID, "descriptor"},
      {"bad_manifest_abi.json", GE_STATUS_PLUGIN_ABI_MISMATCH, "abi"},
      {"bad_dependency.json", GE_STATUS_PLUGIN_MANIFEST_INVALID, "dependencies"},
      {"bad_manifest_fingerprint.json", GE_STATUS_PLUGIN_ABI_MISMATCH, "fingerprint"},
      {"bad_missing_library.json", GE_STATUS_PLUGIN_MANIFEST_INVALID, "manifest"},
  };
  for (const Case& c : cases) {
    auto r = f.Load(c.manifest);
    ASSERT_FALSE(r.ok()) << c.manifest;
    EXPECT_EQ(r.status().code(), c.code) << c.manifest << ": " << r.status().ToString();
    auto ctx = ge::ParseJson(r.status().context_json());
    ASSERT_TRUE(ctx.ok()) << c.manifest;
    EXPECT_EQ(ctx.value->GetString("stage").value_or(""), c.stage) << c.manifest;
    // Already-loaded plugin untouched.
    const auto after = f.registry->List();
    ASSERT_EQ(after.size(), before.size()) << c.manifest;
    EXPECT_EQ(after[0].state, ge::PluginState::kRegistered);
    EXPECT_EQ(f.registry->Resolve(Op("Bad@1.0.0")), std::nullopt) << c.manifest;
  }
  // Every rejection is audited (PLG-8).
  std::size_t rejects = 0;
  for (const auto& a : f.registry->Audit()) rejects += a.action == "reject" ? 1 : 0;
  EXPECT_EQ(rejects, std::size(cases));
  // Sample plugin still creates operators.
  ge::OperatorCreateArgs args{Op("Src@1.0.0"), 1, "src"};
  EXPECT_TRUE(f.registry->Create(args).ok());
}

TEST(PluginRegistryTest, WhitelistAndConflicts) {
  Fixture f;
  // Outside the search path: rejected before anything is opened.
  const fs::path tmp = fs::temp_directory_path() / "ge_plugin_registry_test";
  fs::create_directories(tmp);
  fs::copy_file(f.dir / "sample_plugin.json", tmp / "sample_plugin.json", fs::copy_options::overwrite_existing);
  fs::copy_file(f.dir / "sample_plugin.so", tmp / "sample_plugin.so", fs::copy_options::overwrite_existing);
  auto outside = f.registry->Load(tmp / "sample_plugin.json");
  ASSERT_FALSE(outside.ok());
  EXPECT_EQ(outside.status().code(), GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_NE(outside.status().message().find("search path"), std::string::npos);

  ASSERT_TRUE(f.Load("sample_plugin.json").ok());
  // Same plugin twice: ALREADY_EXISTS (PLG-4 one source per type@version).
  auto again = f.Load("sample_plugin.json");
  ASSERT_FALSE(again.ok());
  EXPECT_EQ(again.status().code(), GE_STATUS_ALREADY_EXISTS);
  EXPECT_EQ(f.registry->List().size(), 1U);
  // Dependency satisfied by the host is accepted.
  Fixture g({{"tensorrt", "9.9.1"}});
  EXPECT_TRUE(g.Load("bad_dependency.json").ok());
}

TEST(PluginRegistryTest, DryRunLoadDoesNotRegister) {
  Fixture f;
  auto r = f.registry->DryRunLoad(f.dir / "sample_plugin.json");
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->operators.size(), 4U);
  EXPECT_TRUE(f.registry->List().empty());
  EXPECT_EQ(f.registry->Resolve(Op("Src@1.0.0")), std::nullopt);
  EXPECT_FALSE(f.registry->DryRunLoad(f.dir / "bad_vtable.json").ok());
}

// ---------------------------------------------------------------------------
// Two versions coexist (PLG-4/5); leases and logical unload (12 §9.2–9.4).
// ---------------------------------------------------------------------------

TEST(PluginRegistryTest, TwoVersionsCoexistAndLeasesTrackReferences) {
  Fixture f;
  auto v1 = f.Load("sample_plugin.json");
  auto v2 = f.Load("sample_plugin_v2.json");
  ASSERT_TRUE(v1.ok() && v2.ok());
  EXPECT_NE(v1->id, v2->id);
  EXPECT_EQ(f.registry->Resolve(Op("Pass@1.0.0")), v1->id);
  EXPECT_EQ(f.registry->Resolve(Op("Pass@2.0.0")), v2->id);

  ge::OperatorCreateArgs a1{Op("Pass@1.0.0"), 7, "p", ge::JsonValue(ge::JsonObject{}), 3, 1};
  ge::OperatorCreateArgs a2{Op("Pass@2.0.0"), 8, "q", ge::JsonValue(ge::JsonObject{}), 4, 1};
  auto op1 = f.registry->Create(a1);
  auto op2 = f.registry->Create(a2);
  ASSERT_TRUE(op1.ok() && op2.ok());
  EXPECT_EQ(f.registry->Get(v1->id)->state, ge::PluginState::kActive);
  EXPECT_EQ(f.registry->Get(v1->id)->references, 1U);
  EXPECT_EQ(f.registry->Get(v2->id)->references, 1U);
  const auto refs = f.registry->SnapshotReferences(Op("Pass@1.0.0"));
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(refs[0].session_id, 3U);
  EXPECT_EQ(refs[0].node_id, 7U);
  EXPECT_EQ(refs[0].plugin_id, v1->id);

  // Retire v1: no new references, existing ones keep working.
  ASSERT_TRUE(f.registry->Retire(v1->id).ok());
  EXPECT_EQ(f.registry->Get(v1->id)->state, ge::PluginState::kRetiring);
  auto refused = f.registry->Create(a1);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.status().code(), GE_STATUS_PLUGIN_RETIRED);
  EXPECT_NE(f.registry->Describe(Op("Pass@1.0.0")), nullptr);  // running graphs still validate
  EXPECT_EQ(f.registry->LogicalUnload(v1->id).code(), GE_STATUS_WOULD_BLOCK);
  ge::OpenRequest open;
  open.session_id = 3;
  open.topology_version = 1;
  EXPECT_TRUE((*op1)->Open(open).ok());
  // Last reference released -> logically unloaded, resolution gone, handle kept.
  op1->reset();
  const auto info = f.registry->Get(v1->id);
  EXPECT_EQ(info->state, ge::PluginState::kLogicallyUnloaded);
  EXPECT_EQ(info->references, 0U);
  EXPECT_EQ(f.registry->Resolve(Op("Pass@1.0.0")), std::nullopt);
  EXPECT_EQ(f.registry->Describe(Op("Pass@1.0.0")), nullptr);
  EXPECT_EQ(f.registry->Create(a1).status().code(), GE_STATUS_NOT_FOUND);
  // v2 unaffected.
  EXPECT_EQ(f.registry->Get(v2->id)->state, ge::PluginState::kActive);
  EXPECT_TRUE(f.registry->Create(a2).ok());
  bool saw_unload = false;
  for (const auto& t : f.transitions) {
    if (t.first == ge::PluginState::kRetiring && t.second == ge::PluginState::kLogicallyUnloaded) saw_unload = true;
  }
  EXPECT_TRUE(saw_unload);
  // Audit trail covers reference/release/retire/unload (PLG-8).
  std::set<std::string> actions;
  for (const auto& a : f.registry->Audit()) actions.insert(a.action);
  for (const char* a : {"load", "reference", "release", "retire", "logical_unload", "state"}) {
    EXPECT_TRUE(actions.contains(a)) << a;
  }
}

TEST(PluginRegistryTest, PhysicalUnloadRequiresLogicalUnloadAndZeroRefs) {
  Fixture f;
  auto v1 = f.Load("sample_plugin.json");
  ASSERT_TRUE(v1.ok());
  ge::OperatorCreateArgs a{Op("Sink@1.0.0"), 1, "s"};
  auto op = f.registry->Create(a);
  ASSERT_TRUE(op.ok());
  // Not unloaded: unsafe, state unchanged.
  EXPECT_EQ(f.registry->PhysicalUnload(v1->id).code(), GE_STATUS_PLUGIN_PHYSICAL_UNLOAD_UNSAFE);
  EXPECT_EQ(f.registry->Get(v1->id)->state, ge::PluginState::kActive);
  ASSERT_TRUE(f.registry->Retire(v1->id).ok());
  EXPECT_EQ(f.registry->PhysicalUnload(v1->id).code(), GE_STATUS_PLUGIN_PHYSICAL_UNLOAD_UNSAFE);
  op->reset();
  EXPECT_EQ(f.registry->Get(v1->id)->state, ge::PluginState::kLogicallyUnloaded);
  // Thread probe veto keeps the logical state.
  f.options.thread_probe = [](const ge::PluginInfo&) { return false; };
  ge::PluginRegistry strict(f.options);
  auto v = strict.Load(f.dir / "sample_plugin.json");
  ASSERT_TRUE(v.ok());
  ASSERT_TRUE(strict.Retire(v->id).ok());
  EXPECT_EQ(strict.Get(v->id)->state, ge::PluginState::kLogicallyUnloaded);
  EXPECT_EQ(strict.PhysicalUnload(v->id).code(), GE_STATUS_PLUGIN_PHYSICAL_UNLOAD_UNSAFE);
  EXPECT_EQ(strict.Get(v->id)->state, ge::PluginState::kLogicallyUnloaded);
  // Default probe: dlclose happens, plugin id can be loaded again.
  ASSERT_TRUE(f.registry->PhysicalUnload(v1->id).ok());
  EXPECT_EQ(f.registry->Get(v1->id)->state, ge::PluginState::kPhysicallyUnloaded);
  EXPECT_TRUE(f.Load("sample_plugin.json").ok());
}

// ---------------------------------------------------------------------------
// Plugin operators inside a Session: emit/EOS/buffer/parameters through the
// C ABI end to end.
// ---------------------------------------------------------------------------

TEST(PluginRegistryTest, PluginGraphRunsToCompletionThroughCAbi) {
  Fixture f;
  ASSERT_TRUE(f.Load("sample_plugin.json").ok());
  ge::GraphBuilder g("plugin");
  auto src = g.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(50)}}));
  auto pass = g.AddNode(Op("Pass@1.0.0"), "pass");
  auto sink = g.AddNode(Op("Sink@1.0.0"), "sink");
  g.Connect(src.port("out"), pass.port("in"), {.id = "e0"});
  g.Connect(pass.port("out"), sink.port("in"), {.id = "e1"});
  ge::ExecutorPool exec(0);
  ge::OperationRegistry ops;
  ge::SessionOptions so;
  so.coordinator_thread = false;
  auto s = ge::Session::Create(*g.Build(), *f.registry, exec, ops, so);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ASSERT_TRUE((*s)->Start().ok());
  for (int i = 0; i < 100000 && !(*s)->WaitStopped(std::chrono::milliseconds(0)); ++i) {
    (void)exec.RunPending();
    (*s)->Tick();
  }
  EXPECT_EQ((*s)->state(), ge::SessionState::kStopped);
  const ge::JsonValue snap = (*s)->Snapshot();
  for (const ge::JsonValue& n : snap.Find("nodes")->as_array()) {
    const auto id = n.GetString("id").value_or("");
    const auto out = n.Find("metrics")->GetInteger("packets_out").value_or(-1);
    const auto in = n.Find("metrics")->GetInteger("packets_in").value_or(-1);
    if (id == "src") {
      EXPECT_EQ(out, 50);
    }
    if (id == "pass") {
      EXPECT_EQ(in, 50);
    }
    if (id == "sink") {
      EXPECT_EQ(in, 50);
    }
  }
  EXPECT_EQ(f.registry->Get(1)->references, 3U);
  s->reset();
  EXPECT_EQ(f.registry->Get(1)->references, 0U);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

TEST(PluginRegistryTest, PluginOpenFailureAndProcessFailureAreReported) {
  Fixture f;
  ASSERT_TRUE(f.Load("sample_plugin.json").ok());
  ge::ExecutorPool exec(0);
  ge::OperationRegistry ops;
  ge::SessionOptions so;
  so.coordinator_thread = false;
  {
    ge::GraphBuilder g("open");
    auto src = g.AddNode(Op("Src@1.0.0"), "src");
    auto sink = g.AddNode(Op("Sink@1.0.0"), "sink", ge::JsonValue(ge::JsonObject{{"fail_open", ge::JsonValue(1)}}));
    g.Connect(src.port("out"), sink.port("in"));
    auto s = ge::Session::Create(*g.Build(), *f.registry, exec, ops, so);
    ASSERT_TRUE(s.ok());
    const ge::Status st = (*s)->Start();
    EXPECT_EQ(st.code(), GE_STATUS_NODE_WARMUP_FAILED);
    EXPECT_NE(st.message().find("fail_open"), std::string::npos);
    EXPECT_EQ((*s)->state(), ge::SessionState::kFailed);
  }
  {
    ge::GraphBuilder g("create");
    auto src = g.AddNode(Op("Src@1.0.0"), "src", ge::JsonValue(ge::JsonObject{{"fail_create", ge::JsonValue(1)}}));
    auto sink = g.AddNode(Op("Sink@1.0.0"), "sink");
    g.Connect(src.port("out"), sink.port("in"));
    auto s = ge::Session::Create(*g.Build(), *f.registry, exec, ops, so);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.status().code(), GE_STATUS_INTERNAL);
    EXPECT_NE(s.status().message().find("fail_create"), std::string::npos);
  }
  EXPECT_EQ(f.registry->Get(1)->references, 0U);
}

}  // namespace
