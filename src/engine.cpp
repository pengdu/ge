#include <ge/cpp/engine.h>

#include <ge/cpp/scheduler.h>
#include <ge/cpp/graph_spec_json.h>
#include <ge/cpp/metrics_export.h>

#include <algorithm>
#include <charconv>

#include "clock.h"
#include "plugin_lifecycle_service.h"
#include "session_manager.h"

namespace ge {

namespace {

Status JsonError(std::string_view what, const std::string& err) {
  return Status::InvalidArgument(std::string(what) + ": " + err);
}

}  // namespace

// ---------------------------------------------------------------------------
// Config / reports
// ---------------------------------------------------------------------------

Result<EngineConfig> EngineConfig::FromJson(const char* plugin_search_paths_json,
                                            const char* resource_limits_json,
                                            const char* observability_config_json) {
  EngineConfig c;
  if (plugin_search_paths_json != nullptr && plugin_search_paths_json[0] != '\0') {
    JsonParseResult parsed = ParseJson(plugin_search_paths_json);
    if (!parsed.ok()) return JsonError("plugin_search_paths_json", parsed.error);
    const JsonValue& doc = *parsed.value;
    const JsonValue* paths = &doc;
    const JsonValue* deps = nullptr;
    if (doc.is_object()) {
      paths = doc.Find("paths");
      deps = doc.Find("host_dependencies");
      if (paths == nullptr) return Status::InvalidArgument("plugin_search_paths_json: missing 'paths'");
    }
    if (!paths->is_array()) return Status::InvalidArgument("plugin_search_paths_json: paths must be an array");
    for (const JsonValue& p : paths->as_array()) {
      if (!p.is_string()) return Status::InvalidArgument("plugin_search_paths_json: path must be a string");
      c.plugin_search_paths.emplace_back(p.as_string());
    }
    if (deps != nullptr) {
      if (!deps->is_object()) return Status::InvalidArgument("host_dependencies must be an object");
      for (const auto& [k, v] : deps->as_object()) {
        if (!v.is_string()) return Status::InvalidArgument("host_dependencies values must be strings");
        c.host_dependencies[k] = v.as_string();
      }
    }
  }
  if (resource_limits_json != nullptr && resource_limits_json[0] != '\0') {
    JsonParseResult parsed = ParseJson(resource_limits_json);
    if (!parsed.ok()) return JsonError("resource_limits_json", parsed.error);
    const JsonValue& doc = *parsed.value;
    if (!doc.is_object()) return Status::InvalidArgument("resource_limits_json must be an object");
    if (const auto t = doc.GetInteger("cpu_threads")) {
      if (*t < 0 || *t > 1024) return Status::InvalidArgument("cpu_threads out of range");
      c.cpu_threads = static_cast<std::uint32_t>(*t);
    }
    if (const auto b = doc.GetInteger("buffer_pool_bytes")) {
      if (*b < 0) return Status::InvalidArgument("buffer_pool_bytes must be >= 0");
      c.buffer_pool.max_cached_bytes = static_cast<std::size_t>(*b);
    }
    if (const auto a = doc.GetBool("admission")) c.resource_admission = *a;
    if (const auto u = doc.GetBool("reject_unbudgeted_edges")) c.reject_unbudgeted_edges = *u;
    if (const JsonValue* r = doc.Find("resources"); r != nullptr) {
      // {"cpu_threads": 8, "host_memory_bytes": 1e9, "gpu_memory@0": ...}
      if (!r->is_object()) return Status::InvalidArgument("resources must be an object");
      for (const auto& [k, v] : r->as_object()) {
        if (!v.is_integer() || v.as_integer() < 0) {
          return Status::InvalidArgument("resources." + k + " must be a non-negative integer");
        }
        std::string_view key = k;
        std::int32_t device = -1;
        if (const auto at = key.rfind('@'); at != std::string_view::npos) {
          const auto tail = key.substr(at + 1);
          int dev = -1;
          if (std::from_chars(tail.data(), tail.data() + tail.size(), dev).ec != std::errc{} || dev < 0) {
            return Status::InvalidArgument("resources." + k + ": bad device suffix");
          }
          device = dev;
          key = key.substr(0, at);
        }
        const auto kind = ParseResourceKind(key);
        if (!kind) return Status::InvalidArgument("resources." + k + ": unknown resource kind");
        if (IsPerDevice(*kind) && device < 0) return Status::InvalidArgument("resources." + k + ": needs @device");
        if (!IsPerDevice(*kind)) device = -1;
        c.resource_capacities.push_back({*kind, device, static_cast<std::uint64_t>(v.as_integer())});
      }
    }
    if (const auto d = doc.GetInteger("drain_timeout_ms")) {
      if (*d <= 0) return Status::InvalidArgument("drain_timeout_ms must be > 0");
      c.default_drain_timeout = std::chrono::milliseconds(*d);
    }
    if (const auto q = doc.GetInteger("completion_queue_capacity")) {
      if (*q <= 0 || *q > (1 << 20)) return Status::InvalidArgument("completion_queue_capacity out of range");
      c.async.completion_queue_capacity = static_cast<std::uint32_t>(*q);
    }
    if (const JsonValue* b = doc.Find("batch"); b != nullptr) {
      if (!b->is_object()) return Status::InvalidArgument("batch must be an object");
      if (const auto e = b->GetBool("enabled")) c.async.batching = *e;
      if (const auto m = b->GetInteger("max_batch")) {
        if (*m < 1 || *m > 1024) return Status::InvalidArgument("batch.max_batch out of range");
        c.async.max_batch = static_cast<std::uint32_t>(*m);
      }
      if (const auto t = b->GetInteger("timeout_ms")) {
        if (*t < 0 || *t > 60000) return Status::InvalidArgument("batch.timeout_ms out of range");
        c.async.batch_timeout = std::chrono::milliseconds(*t);
      }
    }
  }
  if (observability_config_json != nullptr && observability_config_json[0] != '\0') {
    JsonParseResult parsed = ParseJson(observability_config_json);
    if (!parsed.ok()) return JsonError("observability_config_json", parsed.error);
    const JsonValue& doc = *parsed.value;
    if (!doc.is_object()) return Status::InvalidArgument("observability_config_json must be an object");
    if (const auto q = doc.GetInteger("event_queue_capacity")) {
      if (*q <= 0) return Status::InvalidArgument("event_queue_capacity must be > 0");
      c.events.queue_capacity = static_cast<std::size_t>(*q);
    }
    if (const auto t = doc.GetInteger("observer_threads")) {
      if (*t <= 0 || *t > 64) return Status::InvalidArgument("observer_threads out of range");
      c.events.observer_threads = static_cast<std::uint32_t>(*t);
    }
    if (const auto w = doc.GetInteger("watchdog_period_ms")) {
      if (*w <= 0) return Status::InvalidArgument("watchdog_period_ms must be > 0");
      c.watchdog_period = std::chrono::milliseconds(*w);
    }
    if (const auto a = doc.GetInteger("audit_capacity")) {
      if (*a <= 0) return Status::InvalidArgument("audit_capacity must be > 0");
      c.audit_capacity = static_cast<std::size_t>(*a);
    }
  }
  return c;
}

JsonValue SessionUpgradeResult::ToJson() const {
  JsonObject o;
  o["session_id"] = JsonValue(session);
  JsonArray n;
  for (const std::string& id : nodes) n.emplace_back(id);
  o["nodes"] = JsonValue(std::move(n));
  o["operation_id"] = JsonValue(operation);
  o["succeeded"] = JsonValue(result.ok());
  if (!result.ok()) {
    o["code"] = JsonValue(Status::CodeName(result.code()));
    o["message"] = JsonValue(result.message());
  }
  return JsonValue(std::move(o));
}

bool UpgradeReport::all_succeeded() const noexcept {
  return std::all_of(sessions.begin(), sessions.end(),
                     [](const SessionUpgradeResult& r) { return r.result.ok(); });
}

JsonValue UpgradeReport::ToJson() const {
  JsonObject o;
  o["operation_id"] = JsonValue(operation);
  o["old_plugin"] = JsonValue(old_plugin);
  o["new_plugin"] = JsonValue(new_plugin);
  JsonArray s;
  for (const SessionUpgradeResult& r : sessions) s.push_back(r.ToJson());
  o["sessions"] = JsonValue(std::move(s));
  o["all_succeeded"] = JsonValue(all_succeeded());
  o["old_unloaded"] = JsonValue(unloaded);
  return JsonValue(std::move(o));
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Engine>> Engine::Create(EngineConfig config) {
  for (const auto& p : config.plugin_search_paths) {
    std::error_code ec;
    if (!std::filesystem::is_directory(p, ec)) {
      return Status::InvalidArgument("plugin search path '" + p.string() + "' is not a directory");
    }
  }
  return std::unique_ptr<Engine>(new Engine(std::move(config)));
}

namespace {

// AUD-1: one audit record per terminal operation. The operation detail is
// the digest (redacted by AuditLog::Append).
AuditRecord AuditFromOperation(const OperationRecord& r) {
  AuditRecord a;
  a.caller = r.caller;
  a.operation = r.kind;
  a.session_id = r.session_id;
  a.operation_id = r.id;
  a.topology_version = r.topology_version;
  a.parameter_version = r.parameter_version;
  a.result = r.result.code();
  a.message = r.result.message();
  if (r.session_id != 0) {
    a.target = "session:" + std::to_string(r.session_id);
  } else if (const auto p = r.detail.GetInteger("plugin")) {
    a.target = "plugin:" + std::to_string(*p);
  } else {
    a.target = "engine";
  }
  JsonObject d = r.detail.is_object() ? r.detail.as_object() : JsonObject{};
  d["state"] = JsonValue(ToString(r.state));
  if (!r.result.context_json().empty()) {
    if (auto ctx = ParseJson(r.result.context_json()); ctx.ok()) d["error_context"] = *ctx.value;
  }
  a.digest = JsonValue(std::move(d));
  return a;
}

}  // namespace

Engine::Engine(EngineConfig config)
    : config_(std::move(config)),
      pool_(HostBufferPool::Create(config_.buffer_pool)),
      events_(std::make_unique<EventBus>(config_.events)),
      audit_(config_.audit_capacity) {
  operations_.SetTerminalHook([this](const OperationRecord& r) { audit_.Append(AuditFromOperation(r)); });
  PluginRegistryOptions ro;
  ro.search_paths = config_.plugin_search_paths;
  ro.host_dependencies = config_.host_dependencies;
  if (config_.engine_fingerprint) ro.engine_fingerprint = *config_.engine_fingerprint;
  ro.services.pool = pool_;
  ro.services.event_publish = [this](const ge_event& e) {
    Event ev;
    ev.kind = e.kind == GE_EVENT_DATA_PLANE ? EventKind::kDataPlane : EventKind::kObservation;
    ev.type = e.type != nullptr ? e.type : "";
    ev.severity = static_cast<Severity>(e.severity);
    ev.session = e.session_id;
    ev.source_node = e.source_node_id;
    ev.timestamp_ns = e.timestamp_ns;
    if (e.has_seq != 0) ev.seq = e.seq;
    if (e.has_pts != 0) ev.pts_ns = e.pts_ns;
    if (e.detail_json != nullptr) {
      if (auto parsed = ParseJson(e.detail_json); parsed.ok()) ev.detail = *parsed.value;
    }
    events_->Publish(std::move(ev));
  };
  ro.services.log = [this](const ge_log_record& r) {
    Event ev;
    ev.type = "log";
    ev.severity = static_cast<Severity>(r.severity);
    ev.session = r.session_id;
    ev.source_node = r.node_id;
    JsonObject d;
    d["message"] = JsonValue(r.message);
    if (r.fields_json != nullptr) {
      if (auto parsed = ParseJson(r.fields_json); parsed.ok()) d["fields"] = *parsed.value;
    }
    ev.detail = JsonValue(std::move(d));
    events_->Publish(std::move(ev));
  };
  plugins_ = std::make_unique<PluginRegistry>(std::move(ro));
  if (config_.builtin_operators) factory_.Add(config_.builtin_operators.get());
  factory_.Add(plugins_.get());
  plugins_->on_state_changed = [this](const PluginInfo& info, PluginState from, PluginState to) {
    Event ev;
    ev.type = "plugin_state";
    ev.severity = Severity::kInfo;
    JsonObject d;
    d["plugin"] = JsonValue(info.id);
    d["plugin_id"] = JsonValue(info.plugin_id);
    d["from"] = JsonValue(ToString(from));
    d["to"] = JsonValue(ToString(to));
    ev.detail = JsonValue(std::move(d));
    events_->Publish(std::move(ev));
  };
  executor_ = std::make_unique<ExecutorPool>(config_.cpu_threads);
  if (config_.resource_admission) {
    std::vector<ResourceCapacity> caps = ResourceLedger::DefaultCapacities(config_.cpu_threads);
    // Explicit entries override the defaults (same kind/device) or add new ones.
    for (const ResourceCapacity& c : config_.resource_capacities) {
      const auto it = std::find_if(caps.begin(), caps.end(), [&](const ResourceCapacity& d) {
        return d.kind == c.kind && d.device_id == c.device_id;
      });
      if (it == caps.end()) {
        caps.push_back(c);
      } else {
        it->capacity = c.capacity;
      }
    }
    ledger_ = std::make_unique<ResourceLedger>(std::move(caps));
  }
  AsyncOptions ao = config_.async;
  ao.worker_thread = config_.async_worker_thread.value_or(config_.cpu_threads != 0);
  async_ = std::make_unique<AsyncRuntime>(ao);

  PluginLifecycleServices ls;
  ls.plugins = plugins_.get();
  ls.factory = &factory_;
  ls.operations = &operations_;
  ls.audit = &audit_;
  ls.executor = executor_.get();
  ls.async = async_.get();
  ls.publish_event = [this](SessionId session, std::string type, Severity severity, NodeId node,
                            JsonValue detail) {
    PublishSessionEvent(session, std::move(type), severity, node, std::move(detail));
  };
  lifecycle_ = std::make_unique<PluginLifecycleService>(std::move(ls), config_);
  SessionManagerServices ss;
  ss.factory = &factory_;
  ss.executor = executor_.get();
  ss.operations = &operations_;
  ss.async = async_.get();
  ss.ledger = ledger_.get();
  ss.operator_generation = [this] { return plugins_->operator_generation(); };
  sessions_ = std::make_unique<SessionManager>(
      std::move(ss), config_,
      [this](SessionId id, CallerContext caller) { return MakeSessionEvents(id, std::move(caller)); });
  lifecycle_->set_sessions(sessions_.get());

  if (config_.watchdog_thread) watchdog_ = std::thread([this] { WatchdogLoop(); });
}

Engine::~Engine() {
  {
    std::lock_guard lock(watchdog_mutex_);
    stop_watchdog_ = true;
    watchdog_cv_.notify_all();
  }
  if (watchdog_.joinable()) watchdog_.join();
  StopAll(true);
  sessions_->Clear();  // 13 §7.3: sessions before shared services
  async_.reset();
  executor_->Stop();
  events_->Flush();
}

void Engine::WatchdogLoop() {
  std::unique_lock lock(watchdog_mutex_);
  while (!stop_watchdog_) {
    watchdog_cv_.wait_for(lock, config_.watchdog_period);
    if (stop_watchdog_) return;
    lock.unlock();
    Tick();
    lock.lock();
  }
}

void Engine::Tick() {
  sessions_->TickAll();
  if (!async_->options().worker_thread) (void)async_->Pump();
  if (config_.cpu_threads == 0) (void)executor_->RunPending();
  lifecycle_->CompletePendingUnloads();
}

void Engine::PublishSessionEvent(SessionId session, std::string type, Severity severity, NodeId node,
                                 JsonValue detail) {
  Event ev;
  ev.type = std::move(type);
  ev.severity = severity;
  ev.session = session;
  ev.source_node = node;
  ev.detail = std::move(detail);
  ev.timestamp_ns = WallClockNs();
  events_->Publish(std::move(ev));
}

// ---------------------------------------------------------------------------
// Plugins
// ---------------------------------------------------------------------------

Result<PluginInfo> Engine::LoadPlugin(const std::filesystem::path& manifest) {
  return lifecycle_->Load(manifest);
}

Result<OperationId> Engine::RetirePlugin(PluginId id, RetirePluginOptions options, CallerContext caller) {
  return lifecycle_->Retire(id, options, std::move(caller));
}

Result<UpgradeReport> Engine::UpgradeOperator(const OperatorKey& old_key, const OperatorKey& new_key,
                                              CallerContext caller) {
  return lifecycle_->UpgradeOperator(old_key, new_key, std::move(caller));
}

Result<CapabilityDescriptor> Engine::GetCapability(const OperatorKey& key) const {
  return lifecycle_->GetCapability(key);
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

SessionEvents Engine::MakeSessionEvents(SessionId sid, CallerContext caller) {
  SessionEvents ev;
  ev.on_state_changed = [this, sid](SessionState from, SessionState to) {
    PublishSessionEvent(sid, "session_state", Severity::kInfo, 0,
                        JsonValue(JsonObject{{"from", JsonValue(ToString(from))},
                                             {"to", JsonValue(ToString(to))}}));
  };
  ev.on_node_failed = [this, sid, caller](NodeRuntime& node, const Status& status) {
    PublishSessionEvent(sid, "node_failed", Severity::kError, node.id(),
                        JsonValue(JsonObject{{"node", JsonValue(node.external_id())},
                                             {"code", JsonValue(Status::CodeName(status.code()))},
                                             {"message", JsonValue(status.message())}}));
    // AUD-1: node failures are audited under the session creator's context.
    AuditRecord a;
    a.caller = caller;
    a.operation = "node.failed";
    a.target = "node:" + node.external_id();
    a.session_id = sid;
    a.result = status.code();
    a.message = status.message();
    JsonObject d;
    d["node"] = JsonValue(node.external_id());
    d["op"] = JsonValue(node.operator_key().ToString());
    if (!status.context_json().empty()) {
      if (auto ctx = ParseJson(status.context_json()); ctx.ok()) d["error_context"] = *ctx.value;
    }
    a.digest = JsonValue(std::move(d));
    audit_.Append(std::move(a));
  };
  ev.on_topology_published = [this, sid](TopologyVersion v) {
    PublishSessionEvent(sid, "mutation_state", Severity::kInfo, 0,
                        JsonValue(JsonObject{{"published_version", JsonValue(v)}}));
  };
  ev.on_drain_timeout = [this, sid](TopologyVersion v) {
    PublishSessionEvent(sid, "drain_timeout", Severity::kWarning, 0,
                        JsonValue(JsonObject{{"retired_version", JsonValue(v)}}));
  };
  ev.on_operator_event = [this, sid](NodeRuntime& node, std::string type, Severity sev, JsonValue detail) {
    if (detail.is_object()) {
      JsonObject d = detail.as_object();
      d.emplace("node", JsonValue(node.external_id()));
      detail = JsonValue(std::move(d));
    }
    PublishSessionEvent(sid, std::move(type), sev, node.id(), std::move(detail));
  };
  return ev;
}

Result<Session*> Engine::CreateSession(const GraphSpec& spec, CallerContext caller,
                                       OperationId* out_operation) {
  return sessions_->Create(spec, std::move(caller), out_operation, nullptr);
}

Result<Session*> Engine::CreateSession(const GraphSpec& spec, CallerContext caller, OperationId* out_operation,
                                       std::shared_ptr<const ValidatedGraph> prevalidated) {
  return sessions_->Create(spec, std::move(caller), out_operation, std::move(prevalidated));
}

Status Engine::PrevalidateTemplate(GraphTemplate& tmpl) { return sessions_->PrevalidateTemplate(tmpl); }

Result<Session*> Engine::CreateSession(const GraphTemplate& tmpl, const JsonValue& arguments,
                                       std::string_view instance_name, CallerContext caller,
                                       OperationId* out_operation) {
  return sessions_->CreateFromTemplate(tmpl, arguments, instance_name, std::move(caller), out_operation);
}

Session* Engine::FindSession(SessionId id) const { return sessions_->Find(id); }

std::shared_ptr<Session> Engine::FindSessionShared(SessionId id) const { return sessions_->FindShared(id); }

Status Engine::DestroySession(SessionId id) {
  const Status s = sessions_->Destroy(id);
  // Destroy only returns once the last session reference dropped, so the
  // leases are back and any retire waiting on them can finish now.
  if (s.ok()) lifecycle_->CompletePendingUnloads();
  return s;
}

void Engine::StopAll(bool fast) { sessions_->StopAll(fast); }

std::vector<SessionId> Engine::Sessions() const { return sessions_->Ids(); }

std::vector<std::shared_ptr<Session>> Engine::SessionRefs() const { return sessions_->Refs(); }

std::string Engine::RenderPrometheus() { return ge::RenderPrometheus(*this); }

}  // namespace ge
