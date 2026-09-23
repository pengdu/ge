#include <ge/cpp/plugin_registry.h>

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

#include "clock.h"

namespace ge {

namespace {

std::string Ctx(std::string_view stage, const JsonObject& extra = {}) {
  JsonObject o = extra;
  o["stage"] = JsonValue(stage);
  return JsonValue(std::move(o)).Serialize();
}

Status ManifestError(std::string_view stage, std::string message, JsonObject extra = {}) {
  return Status(GE_STATUS_PLUGIN_MANIFEST_INVALID, std::move(message), false,
                Ctx(stage, std::move(extra)));
}

Status AbiError(std::string_view stage, std::string message, JsonObject extra = {}) {
  return Status(GE_STATUS_PLUGIN_ABI_MISMATCH, std::move(message), false,
                Ctx(stage, std::move(extra)));
}

template <typename T>
bool HeaderOk(const T* s) noexcept {
  return s != nullptr && s->header.abi_major == GE_ABI_MAJOR && s->header.struct_size >= sizeof(T);
}

// Host version satisfies a constraint when it equals it or extends it by
// further dotted components ("8.6.1" satisfies "8.6").
bool VersionSatisfies(std::string_view host, std::string_view constraint) {
  if (host == constraint) return true;
  return host.size() > constraint.size() && host.substr(0, constraint.size()) == constraint &&
         host[constraint.size()] == '.';
}

bool IsUnder(const std::filesystem::path& file, const std::filesystem::path& dir) {
  auto f = file.begin();
  for (auto d = dir.begin(); d != dir.end(); ++d, ++f) {
    if (f == file.end() || *f != *d) return false;
  }
  return true;
}

struct LibraryHandle {
  void* handle = nullptr;
  ~LibraryHandle() {
    if (handle != nullptr) dlclose(handle);
  }
  void* Release() noexcept { return std::exchange(handle, nullptr); }
};

}  // namespace

std::string_view ToString(PluginState s) noexcept {
  switch (s) {
    case PluginState::kDiscovered: return "discovered";
    case PluginState::kValidating: return "validating";
    case PluginState::kLoading: return "loading";
    case PluginState::kRegistered: return "registered";
    case PluginState::kActive: return "active";
    case PluginState::kRetiring: return "retiring";
    case PluginState::kLogicallyUnloaded: return "logically_unloaded";
    case PluginState::kPhysicallyUnloaded: return "physically_unloaded";
    case PluginState::kRejected: return "rejected";
  }
  return "unknown";
}

JsonValue PluginInfo::ToJson() const {
  JsonObject o;
  o["id"] = JsonValue(id);
  o["plugin_id"] = JsonValue(plugin_id);
  o["manifest_path"] = JsonValue(manifest_path.string());
  o["library_path"] = JsonValue(library_path.string());
  o["build_fingerprint"] = JsonValue(build_fingerprint);
  o["state"] = JsonValue(ToString(state));
  JsonArray ops;
  for (const OperatorKey& k : operators) ops.emplace_back(k.ToString());
  o["operators"] = JsonValue(std::move(ops));
  o["references"] = JsonValue(static_cast<std::uint64_t>(references));
  if (!rejection.ok()) {
    JsonObject r;
    r["code"] = JsonValue(Status::CodeName(rejection.code()));
    r["message"] = JsonValue(rejection.message());
    if (!rejection.context_json().empty()) {
      if (auto parsed = ParseJson(rejection.context_json()); parsed.ok()) r["context"] = *parsed.value;
    }
    o["rejection"] = JsonValue(std::move(r));
  }
  return JsonValue(std::move(o));
}

JsonValue PluginReference::ToJson() const {
  JsonObject o;
  o["plugin_id"] = JsonValue(plugin_id);
  o["operator"] = JsonValue(key.ToString());
  o["session_id"] = JsonValue(session_id);
  o["node_id"] = JsonValue(node_id);
  o["topology_version"] = JsonValue(topology_version);
  return JsonValue(std::move(o));
}

JsonValue PluginAuditRecord::ToJson() const {
  JsonObject o;
  o["timestamp_ns"] = JsonValue(timestamp_ns);
  o["plugin_id"] = JsonValue(plugin_id);
  o["action"] = JsonValue(action);
  o["detail"] = detail;
  return JsonValue(std::move(o));
}

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

struct PluginRegistry::Plugin {
  PluginId id = 0;
  PluginState state = PluginState::kDiscovered;
  Loaded loaded;
  std::vector<PluginReference> references;
  std::uint32_t reference_count = 0;
  Status rejection;
};

struct PluginRegistry::State {
  std::mutex mutex;
  std::map<PluginId, std::shared_ptr<Plugin>> plugins;
  std::map<OperatorKey, PluginId> keys;
  std::vector<PluginAuditRecord> audit;
  std::size_t audit_capacity = 4096;
  PluginId next_id = 1;
  // Bumped on every change to |keys|. Lives here (not on PluginRegistry) so
  // the lease release lambda -- which only holds this State -- can bump it
  // when the last reference to a retiring plugin drops its keys.
  std::atomic<std::uint64_t> operator_generation{0};
  std::function<void(const PluginInfo&, PluginState, PluginState)>* on_state_changed = nullptr;

  void Append(PluginId id, std::string action, JsonValue detail) {  // mutex held
    if (audit.size() >= audit_capacity) audit.erase(audit.begin());
    audit.push_back(PluginAuditRecord{WallClockNs(), id, std::move(action), std::move(detail)});
  }
};

// 12 §9.3: one per live operator instance. Releasing the last reference of
// a retiring plugin completes its logical unload (12 §9.4).
struct PluginRegistry::Lease {
  std::shared_ptr<State> state;
  PluginId plugin = 0;
  PluginReference ref;
  std::function<void()> on_release;
  ~Lease() {
    if (on_release) on_release();
  }
};

PluginInfo PluginRegistry::InfoOf(const Plugin& p) {
  PluginInfo info;
  info.id = p.id;
  info.plugin_id = p.loaded.manifest.plugin_id;
  info.manifest_path = p.loaded.manifest_path;
  info.library_path = p.loaded.library_path;
  info.build_fingerprint = p.loaded.manifest.build_fingerprint;
  info.state = p.state;
  for (const OperatorEntry& e : p.loaded.operators) info.operators.push_back(e.key);
  info.references = p.reference_count;
  info.rejection = p.rejection;
  return info;
}

void PluginRegistry::Transition(Plugin& p, PluginState to) {
  const PluginState from = p.state;
  if (from == to) return;
  p.state = to;
  state_->Append(p.id, "state", JsonValue(JsonObject{{"from", JsonValue(ToString(from))},
                                                      {"to", JsonValue(ToString(to))}}));
}

PluginRegistry::PluginRegistry(PluginRegistryOptions options)
    : options_(std::move(options)), state_(std::make_shared<State>()) {
  state_->audit_capacity = options_.audit_capacity;
  state_->on_state_changed = &on_state_changed;
}

PluginRegistry::~PluginRegistry() {
  std::lock_guard lock(state_->mutex);
  state_->on_state_changed = nullptr;
  // Handles are kept open (12 §9.4 default): leases may still be alive in
  // sessions being torn down; dlclose only happens through PhysicalUnload.
}

// ---------------------------------------------------------------------------
// Validation chain (12 §9.1)
// ---------------------------------------------------------------------------

Status PluginRegistry::CheckWhitelist(const std::filesystem::path& manifest_path) const {
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(manifest_path, ec);
  if (ec) return ManifestError("whitelist", "cannot resolve manifest path: " + manifest_path.string());
  for (const auto& dir : options_.search_paths) {
    const auto cdir = std::filesystem::weakly_canonical(dir, ec);
    if (ec) continue;
    if (IsUnder(canonical, cdir)) return Status::Ok();
  }
  return ManifestError("whitelist", "manifest '" + manifest_path.string() +
                                        "' is outside every plugin search path");
}

Status PluginRegistry::CheckDependencies(const PluginManifest& manifest) const {
  for (const PluginDependency& dep : manifest.dependencies) {
    const auto it = options_.host_dependencies.find(dep.name);
    if (it == options_.host_dependencies.end()) {
      return ManifestError("dependencies", "host does not provide dependency '" + dep.name + "'",
                           {{"dependency", JsonValue(dep.name)}});
    }
    if (!VersionSatisfies(it->second, dep.version)) {
      return ManifestError("dependencies",
                           "dependency '" + dep.name + "' requires " + dep.version + ", host has " +
                               it->second,
                           {{"dependency", JsonValue(dep.name)}, {"required", JsonValue(dep.version)},
                            {"available", JsonValue(it->second)}});
    }
  }
  return Status::Ok();
}

Status PluginRegistry::CrossCheck(const PluginManifest& manifest, const ge_plugin_descriptor& d,
                                  std::vector<OperatorEntry>* out) const {
  if (d.plugin_id == nullptr || manifest.plugin_id != d.plugin_id) {
    return ManifestError("descriptor", "descriptor plugin_id does not match manifest",
                         {{"manifest", JsonValue(manifest.plugin_id)},
                          {"descriptor", JsonValue(d.plugin_id != nullptr ? d.plugin_id : "")}});
  }
  if (d.build_fingerprint == nullptr || manifest.build_fingerprint != d.build_fingerprint) {
    return AbiError("fingerprint", "descriptor build_fingerprint does not match manifest",
                    {{"manifest", JsonValue(manifest.build_fingerprint)},
                     {"descriptor", JsonValue(d.build_fingerprint != nullptr ? d.build_fingerprint : "")}});
  }
  if (d.manifest_json != nullptr) {
    auto embedded = PluginManifest::ParseJson(d.manifest_json);
    if (!embedded.ok()) {
      return ManifestError("descriptor", "embedded manifest invalid: " + embedded.status().message());
    }
    if (embedded->plugin_id != manifest.plugin_id || embedded->abi != manifest.abi ||
        embedded->operators != manifest.operators ||
        embedded->build_fingerprint != manifest.build_fingerprint ||
        embedded->max_inference_ms != manifest.max_inference_ms) {
      return ManifestError("descriptor", "embedded manifest disagrees with manifest file");
    }
  }
  if (d.operator_count == 0 || d.operators == nullptr) {
    return ManifestError("descriptor", "descriptor declares no operators");
  }
  std::set<OperatorKey> seen;
  for (uint32_t i = 0; i < d.operator_count; ++i) {
    const ge_operator_descriptor& od = d.operators[i];
    if (!HeaderOk(&od)) return AbiError("descriptor", "operator descriptor header mismatch");
    if (od.type_name == nullptr || od.semantic_version == nullptr) {
      return ManifestError("descriptor", "operator descriptor without type/version");
    }
    OperatorEntry e;
    e.key = OperatorKey{od.type_name, od.semantic_version};
    const std::string key = e.key.ToString();
    if (!OperatorKey::Parse(key)) return ManifestError("descriptor", "invalid operator key '" + key + "'");
    if (!seen.insert(e.key).second) {
      return ManifestError("descriptor", "duplicate operator '" + key + "' in descriptor");
    }
    if (std::find(manifest.operators.begin(), manifest.operators.end(), e.key) ==
        manifest.operators.end()) {
      return ManifestError("descriptor", "operator '" + key + "' not listed in manifest",
                           {{"operator", JsonValue(key)}});
    }
    if (od.capability_json == nullptr) {
      return ManifestError("descriptor", "operator '" + key + "' has no capability_json");
    }
    auto cap = CapabilityDescriptor::ParseJson(od.capability_json);
    if (!cap.ok()) {
      return ManifestError("descriptor", "operator '" + key + "' capability: " + cap.status().message(),
                           {{"operator", JsonValue(key)}});
    }
    if (cap->op != e.key) {
      return ManifestError("descriptor", "operator '" + key + "' capability names " + cap->op.ToString(),
                           {{"operator", JsonValue(key)}});
    }
    const bool stateful = (od.flags & GE_OPERATOR_FLAG_STATEFUL) != 0;
    const bool async = (od.flags & GE_OPERATOR_FLAG_ASYNC) != 0;
    if (stateful != cap->execution.stateful || async != cap->execution.async) {
      return ManifestError("descriptor", "operator '" + key + "' flags disagree with capability",
                           {{"operator", JsonValue(key)}});
    }
    if (od.max_parallelism != cap->execution.max_parallelism) {
      return ManifestError("descriptor", "operator '" + key + "' max_parallelism disagrees with capability",
                           {{"operator", JsonValue(key)},
                            {"descriptor", JsonValue(static_cast<std::uint64_t>(od.max_parallelism))},
                            {"capability", JsonValue(static_cast<std::uint64_t>(cap->execution.max_parallelism))}});
    }
    if (od.max_inference_ms != 0) {
      if (cap->execution.max_inference_ms && *cap->execution.max_inference_ms != od.max_inference_ms) {
        return ManifestError("descriptor", "operator '" + key + "' max_inference_ms disagrees with capability",
                             {{"operator", JsonValue(key)}});
      }
      if (manifest.max_inference_ms && od.max_inference_ms > *manifest.max_inference_ms) {
        return ManifestError("descriptor", "operator '" + key + "' max_inference_ms exceeds manifest limit",
                             {{"operator", JsonValue(key)}});
      }
      if (!cap->execution.max_inference_ms) cap->execution.max_inference_ms = od.max_inference_ms;
    }
    if (od.parameter_schema_json != nullptr) {
      auto schema = ParseJson(od.parameter_schema_json);
      if (!schema.ok() || !schema.value->is_object()) {
        return ManifestError("descriptor", "operator '" + key + "' parameter_schema_json is not a JSON object");
      }
      if (cap->parameters.schema.is_object() && cap->parameters.schema.as_object().empty()) {
        cap->parameters.schema = *schema.value;
      } else if (cap->parameters.schema != *schema.value) {
        return ManifestError("descriptor", "operator '" + key + "' parameter schema disagrees with capability");
      }
    }
    if (od.functional_description != nullptr && cap->description.empty()) {
      cap->description = od.functional_description;
    }
    if (!HeaderOk(od.vtable)) return AbiError("vtable", "operator '" + key + "' vtable header mismatch");
    if (od.vtable->create == nullptr || od.vtable->open == nullptr || od.vtable->process == nullptr ||
        od.vtable->close == nullptr || od.vtable->destroy == nullptr) {
      return AbiError("vtable", "operator '" + key + "' vtable lacks mandatory entries");
    }
    if (async && od.vtable->submit == nullptr) {
      return AbiError("vtable", "async operator '" + key + "' has no submit entry");
    }
    e.capability = std::make_unique<CapabilityDescriptor>(std::move(*cap));
    e.vtable = od.vtable;
    e.flags = od.flags;
    out->push_back(std::move(e));
  }
  if (seen.size() != manifest.operators.size()) {
    return ManifestError("descriptor", "manifest lists operators the descriptor does not provide");
  }
  return Status::Ok();
}

Result<PluginRegistry::Loaded> PluginRegistry::Validate(const std::filesystem::path& manifest_path,
                                                        std::string* stage) const {
  *stage = "whitelist";
  if (Status s = CheckWhitelist(manifest_path); !s.ok()) return s;

  *stage = "manifest";
  std::ifstream in(manifest_path, std::ios::binary);
  if (!in) return ManifestError("manifest", "cannot read manifest '" + manifest_path.string() + "'");
  std::stringstream buf;
  buf << in.rdbuf();
  auto manifest = PluginManifest::ParseJson(buf.str());
  if (!manifest.ok()) return manifest.status();
  Loaded loaded;
  loaded.manifest = std::move(*manifest);
  loaded.manifest_path = manifest_path;
  const std::filesystem::path dir = manifest_path.parent_path();
  loaded.library_path = dir / loaded.manifest.library;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(loaded.library_path, ec)) {
    return ManifestError("manifest", "library '" + loaded.library_path.string() + "' does not exist");
  }
  for (const std::string& res : loaded.manifest.resources) {
    if (!std::filesystem::exists(dir / res, ec)) {
      return ManifestError("manifest", "resource '" + res + "' does not exist",
                           {{"resource", JsonValue(res)}});
    }
  }

  *stage = "abi";
  if (loaded.manifest.abi.major != GE_ABI_MAJOR) {
    return AbiError("abi", "manifest ABI major " + std::to_string(loaded.manifest.abi.major) +
                               " != engine " + std::to_string(GE_ABI_MAJOR),
                    {{"manifest", JsonValue(static_cast<std::uint64_t>(loaded.manifest.abi.major))},
                     {"engine", JsonValue(static_cast<std::uint64_t>(GE_ABI_MAJOR))}});
  }
  if (loaded.manifest.build_fingerprint != options_.engine_fingerprint) {
    return AbiError("fingerprint", "manifest build_fingerprint does not match the engine",
                    {{"manifest", JsonValue(loaded.manifest.build_fingerprint)},
                     {"engine", JsonValue(options_.engine_fingerprint)}});
  }

  *stage = "dependencies";
  if (Status s = CheckDependencies(loaded.manifest); !s.ok()) return s;

  *stage = "dlopen";
  LibraryHandle lib;
  lib.handle = dlopen(loaded.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (lib.handle == nullptr) {
    const char* err = dlerror();
    return AbiError("dlopen", "dlopen failed: " + std::string(err != nullptr ? err : "unknown"));
  }

  *stage = "descriptor";
  void* sym = dlsym(lib.handle, "ge_plugin_get_descriptor");
  if (sym == nullptr) return AbiError("symbols", "library does not export ge_plugin_get_descriptor");
  auto get_descriptor = reinterpret_cast<ge_plugin_get_descriptor_fn>(sym);
  const ge_plugin_descriptor* d = get_descriptor(GE_ABI_MAJOR);
  if (d == nullptr) {
    return AbiError("abi", "plugin refused ABI major " + std::to_string(GE_ABI_MAJOR));
  }
  if (!HeaderOk(d)) return AbiError("abi", "plugin descriptor header mismatch");
  if (Status s = CrossCheck(loaded.manifest, *d, &loaded.operators); !s.ok()) return s;
  loaded.descriptor = d;
  loaded.handle = lib.Release();
  return loaded;
}

Result<PluginInfo> PluginRegistry::DryRunLoad(const std::filesystem::path& manifest_path) const {
  std::string stage;
  auto loaded = Validate(manifest_path, &stage);
  if (!loaded.ok()) return loaded.status();
  Plugin p;
  p.loaded = std::move(*loaded);
  p.state = PluginState::kRegistered;
  PluginInfo info = InfoOf(p);
  dlclose(p.loaded.handle);
  return info;
}

Result<PluginInfo> PluginRegistry::Load(const std::filesystem::path& manifest_path) {
  std::string stage;
  auto loaded = Validate(manifest_path, &stage);
  std::lock_guard lock(state_->mutex);
  const auto reject = [&](const Status& s) -> Result<PluginInfo> {
    state_->Append(0, "reject",
                   JsonValue(JsonObject{{"manifest", JsonValue(manifest_path.string())},
                                        {"stage", JsonValue(stage)},
                                        {"code", JsonValue(Status::CodeName(s.code()))},
                                        {"message", JsonValue(s.message())}}));
    return s;
  };
  if (!loaded.ok()) return reject(loaded.status());
  LibraryHandle lib;
  lib.handle = loaded->handle;
  stage = "conflicts";
  for (const auto& [id, p] : state_->plugins) {
    const bool live = p->state == PluginState::kRegistered || p->state == PluginState::kActive ||
                      p->state == PluginState::kRetiring;
    if (live && p->loaded.manifest.plugin_id == loaded->manifest.plugin_id) {
      return reject(Status(GE_STATUS_ALREADY_EXISTS,
                           "plugin '" + loaded->manifest.plugin_id + "' is already loaded", false,
                           Ctx(stage, {{"plugin", JsonValue(id)}})));
    }
  }
  for (const OperatorEntry& e : loaded->operators) {
    if (const auto it = state_->keys.find(e.key); it != state_->keys.end()) {
      return reject(Status(GE_STATUS_ALREADY_EXISTS,
                           "operator '" + e.key.ToString() + "' is already registered by plugin " +
                               std::to_string(it->second),
                           false, Ctx(stage, {{"operator", JsonValue(e.key.ToString())},
                                              {"plugin", JsonValue(it->second)}})));
    }
  }
  auto p = std::make_shared<Plugin>();
  p->id = state_->next_id++;
  p->loaded = std::move(*loaded);
  p->loaded.handle = lib.Release();
  p->state = PluginState::kRegistered;
  for (const OperatorEntry& e : p->loaded.operators) state_->keys[e.key] = p->id;
  state_->plugins[p->id] = p;
  BumpOperatorGeneration(&state_->operator_generation);
  JsonArray ops;
  for (const OperatorEntry& e : p->loaded.operators) ops.emplace_back(e.key.ToString());
  state_->Append(p->id, "load",
                 JsonValue(JsonObject{{"plugin_id", JsonValue(p->loaded.manifest.plugin_id)},
                                      {"manifest", JsonValue(manifest_path.string())},
                                      {"library", JsonValue(p->loaded.library_path.string())},
                                      {"fingerprint", JsonValue(p->loaded.manifest.build_fingerprint)},
                                      {"operators", JsonValue(std::move(ops))}}));
  return InfoOf(*p);
}

// ---------------------------------------------------------------------------
// Lifecycle (12 §9.2 / §9.4)
// ---------------------------------------------------------------------------

Status PluginRegistry::Retire(PluginId id) {
  std::vector<std::pair<PluginInfo, std::pair<PluginState, PluginState>>> notify;
  {
    std::lock_guard lock(state_->mutex);
    const auto it = state_->plugins.find(id);
    if (it == state_->plugins.end()) return Status::NotFound("plugin " + std::to_string(id));
    Plugin& p = *it->second;
    if (p.state == PluginState::kRetiring) return Status::Ok();
    if (p.state != PluginState::kRegistered && p.state != PluginState::kActive) {
      return Status::PluginRetired("plugin " + std::to_string(id) + " is " + std::string(ToString(p.state)));
    }
    const PluginState from = p.state;
    Transition(p, PluginState::kRetiring);
    state_->Append(id, "retire", JsonValue(JsonObject{{"references", JsonValue(static_cast<std::uint64_t>(p.reference_count))}}));
    notify.emplace_back(InfoOf(p), std::make_pair(from, PluginState::kRetiring));
    if (p.reference_count == 0) {
      for (const OperatorEntry& e : p.loaded.operators) state_->keys.erase(e.key);
      BumpOperatorGeneration(&state_->operator_generation);
      Transition(p, PluginState::kLogicallyUnloaded);
      state_->Append(id, "logical_unload", JsonValue(JsonObject{}));
      notify.emplace_back(InfoOf(p), std::make_pair(PluginState::kRetiring, PluginState::kLogicallyUnloaded));
    }
  }
  if (on_state_changed) {
    for (const auto& [info, ft] : notify) on_state_changed(info, ft.first, ft.second);
  }
  return Status::Ok();
}

Status PluginRegistry::LogicalUnload(PluginId id) {
  std::optional<PluginInfo> notify;
  PluginState from = PluginState::kRetiring;
  {
    std::lock_guard lock(state_->mutex);
    const auto it = state_->plugins.find(id);
    if (it == state_->plugins.end()) return Status::NotFound("plugin " + std::to_string(id));
    Plugin& p = *it->second;
    if (p.state == PluginState::kLogicallyUnloaded || p.state == PluginState::kPhysicallyUnloaded) {
      return Status::Ok();
    }
    if (p.state != PluginState::kRetiring && p.state != PluginState::kRegistered &&
        p.state != PluginState::kActive) {
      return Status::PluginRetired("plugin " + std::to_string(id) + " is " + std::string(ToString(p.state)));
    }
    if (p.reference_count != 0) {
      return Status(GE_STATUS_WOULD_BLOCK,
                    "plugin " + std::to_string(id) + " still has " +
                        std::to_string(p.reference_count) + " references",
                    true, Ctx("logical_unload", {{"references", JsonValue(static_cast<std::uint64_t>(p.reference_count))}}));
    }
    from = p.state;
    for (const OperatorEntry& e : p.loaded.operators) state_->keys.erase(e.key);
    BumpOperatorGeneration(&state_->operator_generation);
    Transition(p, PluginState::kLogicallyUnloaded);
    state_->Append(id, "logical_unload", JsonValue(JsonObject{}));
    notify = InfoOf(p);
  }
  if (on_state_changed && notify) on_state_changed(*notify, from, PluginState::kLogicallyUnloaded);
  return Status::Ok();
}

Status PluginRegistry::PhysicalUnload(PluginId id) {
  std::optional<PluginInfo> notify;
  {
    std::lock_guard lock(state_->mutex);
    const auto it = state_->plugins.find(id);
    if (it == state_->plugins.end()) return Status::NotFound("plugin " + std::to_string(id));
    Plugin& p = *it->second;
    if (p.state == PluginState::kPhysicallyUnloaded) return Status::Ok();
    const auto unsafe = [&](std::string why) {
      state_->Append(id, "physical_unload_refused", JsonValue(JsonObject{{"reason", JsonValue(why)}}));
      return Status::PluginPhysicalUnloadUnsafe("plugin " + std::to_string(id) + ": " + why);
    };
    if (p.state != PluginState::kLogicallyUnloaded) return unsafe("not logically unloaded");
    if (p.reference_count != 0) return unsafe("references remain");
    if (!p.loaded.manifest.no_owned_threads) return unsafe("plugin may own threads");
    if (options_.thread_probe && !options_.thread_probe(InfoOf(p))) return unsafe("thread probe failed");
    if (p.loaded.handle != nullptr) dlclose(p.loaded.handle);
    p.loaded.handle = nullptr;
    p.loaded.descriptor = nullptr;
    for (OperatorEntry& e : p.loaded.operators) e.vtable = nullptr;
    Transition(p, PluginState::kPhysicallyUnloaded);
    state_->Append(id, "physical_unload", JsonValue(JsonObject{}));
    notify = InfoOf(p);
  }
  if (on_state_changed && notify) {
    on_state_changed(*notify, PluginState::kLogicallyUnloaded, PluginState::kPhysicallyUnloaded);
  }
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<PluginInfo> PluginRegistry::Get(PluginId id) const {
  std::lock_guard lock(state_->mutex);
  const auto it = state_->plugins.find(id);
  if (it == state_->plugins.end()) return std::nullopt;
  return InfoOf(*it->second);
}

std::vector<PluginInfo> PluginRegistry::List() const {
  std::lock_guard lock(state_->mutex);
  std::vector<PluginInfo> out;
  for (const auto& [id, p] : state_->plugins) out.push_back(InfoOf(*p));
  return out;
}

std::uint64_t PluginRegistry::operator_generation() const noexcept {
  return state_->operator_generation.load(std::memory_order_acquire);
}

std::optional<PluginId> PluginRegistry::Resolve(const OperatorKey& key) const {
  std::lock_guard lock(state_->mutex);
  const auto it = state_->keys.find(key);
  if (it == state_->keys.end()) return std::nullopt;
  return it->second;
}

std::vector<PluginReference> PluginRegistry::SnapshotReferences(PluginId id) const {
  std::lock_guard lock(state_->mutex);
  const auto it = state_->plugins.find(id);
  if (it == state_->plugins.end()) return {};
  return it->second->references;
}

std::vector<PluginReference> PluginRegistry::SnapshotReferences(const OperatorKey& key) const {
  std::lock_guard lock(state_->mutex);
  std::vector<PluginReference> out;
  for (const auto& [id, p] : state_->plugins) {
    for (const PluginReference& r : p->references) {
      if (r.key == key) out.push_back(r);
    }
  }
  return out;
}

std::vector<PluginAuditRecord> PluginRegistry::Audit() const {
  std::lock_guard lock(state_->mutex);
  return state_->audit;
}

// ---------------------------------------------------------------------------
// OperatorFactory
// ---------------------------------------------------------------------------

const CapabilityDescriptor* PluginRegistry::Describe(const OperatorKey& key) const {
  std::lock_guard lock(state_->mutex);
  const auto it = state_->keys.find(key);
  if (it == state_->keys.end()) return nullptr;
  const auto pit = state_->plugins.find(it->second);
  if (pit == state_->plugins.end()) return nullptr;
  for (const OperatorEntry& e : pit->second->loaded.operators) {
    if (e.key == key) return e.capability.get();
  }
  return nullptr;
}

Result<std::unique_ptr<Operator>> PluginRegistry::Create(const OperatorCreateArgs& args) {
  const ge_operator_vtable* vtable = nullptr;
  bool is_source = false;
  std::shared_ptr<Lease> lease;
  std::optional<std::pair<PluginInfo, PluginState>> notify;
  std::weak_ptr<State> weak_state = state_;
  {
    std::lock_guard lock(state_->mutex);
    const auto it = state_->keys.find(args.key);
    if (it == state_->keys.end()) return Status::NotFound("operator '" + args.key.ToString() + "' not found");
    const auto pit = state_->plugins.find(it->second);
    if (pit == state_->plugins.end()) return Status::NotFound("operator '" + args.key.ToString() + "' not found");
    const std::shared_ptr<Plugin>& p = pit->second;
    if (p->state != PluginState::kRegistered && p->state != PluginState::kActive) {
      return Status::PluginRetired("operator '" + args.key.ToString() + "' belongs to plugin " +
                                   std::to_string(p->id) + " which is " + std::string(ToString(p->state)));
    }
    for (const OperatorEntry& e : p->loaded.operators) {
      if (e.key == args.key) {
        vtable = e.vtable;
        is_source = e.capability->inputs.empty();
      }
    }
    if (vtable == nullptr) return Status::Internal("operator entry without vtable");
    PluginReference ref{p->id, args.key, args.session_id, args.node_id, args.topology_version};
    p->references.push_back(ref);
    ++p->reference_count;
    state_->Append(p->id, "reference", ref.ToJson());
    if (p->state == PluginState::kRegistered) {
      Transition(*p, PluginState::kActive);
      notify = std::make_pair(InfoOf(*p), PluginState::kRegistered);
    }
    lease = std::make_shared<Lease>();
    lease->state = state_;
    lease->plugin = p->id;
    lease->ref = ref;
    const PluginId pid = p->id;
    lease->on_release = [weak_state, pid, ref] {
      const std::shared_ptr<State> st = weak_state.lock();
      if (!st) return;
      std::optional<PluginInfo> info;
      std::function<void(const PluginInfo&, PluginState, PluginState)> cb;
      {
        std::lock_guard l(st->mutex);
        const auto pi = st->plugins.find(pid);
        if (pi == st->plugins.end()) return;
        Plugin& pl = *pi->second;
        auto r = std::find_if(pl.references.begin(), pl.references.end(), [&](const PluginReference& x) {
          return x.session_id == ref.session_id && x.node_id == ref.node_id &&
                 x.topology_version == ref.topology_version && x.key == ref.key;
        });
        if (r != pl.references.end()) pl.references.erase(r);
        if (pl.reference_count > 0) --pl.reference_count;
        st->Append(pid, "release", ref.ToJson());
        if (pl.state == PluginState::kRetiring && pl.reference_count == 0) {
          for (const OperatorEntry& e : pl.loaded.operators) st->keys.erase(e.key);
          BumpOperatorGeneration(&st->operator_generation);
          pl.state = PluginState::kLogicallyUnloaded;
          st->Append(pid, "state", JsonValue(JsonObject{{"from", JsonValue("retiring")},
                                                        {"to", JsonValue("logically_unloaded")}}));
          st->Append(pid, "logical_unload", JsonValue(JsonObject{}));
          info = InfoOf(pl);
          if (st->on_state_changed != nullptr) cb = *st->on_state_changed;
        }
      }
      if (info && cb) cb(*info, PluginState::kRetiring, PluginState::kLogicallyUnloaded);
    };
  }
  if (notify && on_state_changed) on_state_changed(notify->first, notify->second, PluginState::kActive);
  auto op = PluginOperator::Create(*vtable, args, is_source, options_.services, std::shared_ptr<void>(lease));
  if (!op.ok()) return op.status();
  return std::unique_ptr<Operator>(std::move(*op));
}

}  // namespace ge
