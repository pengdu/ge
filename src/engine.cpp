#include <ge/cpp/engine.h>

#include <algorithm>
#include <set>

#include "clock.h"

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

Engine::Engine(EngineConfig config)
    : config_(std::move(config)),
      pool_(HostBufferPool::Create(config_.buffer_pool)),
      events_(std::make_unique<EventBus>(config_.events)) {
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
  AsyncOptions ao = config_.async;
  ao.worker_thread = config_.async_worker_thread.value_or(config_.cpu_threads != 0);
  async_ = std::make_unique<AsyncRuntime>(ao);
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
  {
    std::lock_guard lock(sessions_mutex_);
    sessions_.clear();  // 13 §7.3: sessions before shared services
  }
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
  std::vector<Session*> sessions;
  {
    std::lock_guard lock(sessions_mutex_);
    for (auto& [id, s] : sessions_) sessions.push_back(s.get());
  }
  for (Session* s : sessions) s->Tick();
  if (!async_->options().worker_thread) (void)async_->Pump();
  if (config_.cpu_threads == 0) (void)executor_->RunPending();
  CompletePendingUnloads();
}

void Engine::PublishSessionEvent(SessionId session, std::string type, Severity severity,
                                 NodeId node, JsonValue detail) {
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
  return plugins_->Load(manifest);
}

Result<OperationId> Engine::RetirePlugin(PluginId id, RetirePluginOptions options,
                                         CallerContext caller) {
  const auto info = plugins_->Get(id);
  if (!info) return Status::NotFound("plugin " + std::to_string(id));
  const OperationId op = operations_.Create(
      "plugin.retire", 0, std::move(caller),
      JsonValue(JsonObject{{"plugin", JsonValue(id)},
                           {"physical", JsonValue(options.request_physical_unload)}}));
  if (Status s = plugins_->Retire(id); !s.ok()) {
    operations_.Fail(op, s);
    return op;
  }
  operations_.SetRunning(op);
  {
    std::lock_guard lock(retire_mutex_);
    pending_retires_.push_back(PendingRetire{id, op, options.request_physical_unload});
  }
  CompletePendingUnloads();
  return op;
}

void Engine::CompletePendingUnloads() {
  std::lock_guard pass(unload_pass_mutex_);
  std::vector<PendingRetire> pending;
  {
    std::lock_guard lock(retire_mutex_);
    pending.swap(pending_retires_);
  }
  std::vector<PendingRetire> keep;
  for (const PendingRetire& r : pending) {
    const Status logical = plugins_->LogicalUnload(r.plugin);
    if (logical.code() == GE_STATUS_WOULD_BLOCK) {
      keep.push_back(r);
      continue;
    }
    if (!logical.ok()) {
      operations_.Fail(r.operation, logical);
      continue;
    }
    JsonObject d;
    d["logically_unloaded"] = JsonValue(true);
    if (r.physical) {
      const Status physical = plugins_->PhysicalUnload(r.plugin);
      d["physically_unloaded"] = JsonValue(physical.ok());
      if (!physical.ok()) {
        // 12 §9.4: logical unload stands; the physical refusal is the result.
        d["physical_unload"] = JsonValue(JsonObject{{"code", JsonValue(Status::CodeName(physical.code()))},
                                                    {"message", JsonValue(physical.message())}});
        operations_.Fail(r.operation, physical, JsonValue(std::move(d)));
        continue;
      }
    }
    operations_.Succeed(r.operation, std::nullopt, std::nullopt, JsonValue(std::move(d)));
  }
  std::lock_guard lock(retire_mutex_);
  pending_retires_.insert(pending_retires_.end(), keep.begin(), keep.end());
}

Result<CapabilityDescriptor> Engine::GetCapability(const OperatorKey& key) const {
  const CapabilityDescriptor* cap = factory_.Describe(key);
  if (cap == nullptr) return Status::NotFound("operator '" + key.ToString() + "' not found");
  return *cap;
}

// 12 §9.3
Result<UpgradeReport> Engine::UpgradeOperator(const OperatorKey& old_key, const OperatorKey& new_key,
                                              CallerContext caller) {
  if (old_key == new_key) return Status::InvalidArgument("old and new operator keys are identical");
  const auto old_plugin = plugins_->Resolve(old_key);
  if (!old_plugin) return Status::NotFound("operator '" + old_key.ToString() + "' not found");
  const auto new_plugin = plugins_->Resolve(new_key);
  if (!new_plugin) return Status::NotFound("operator '" + new_key.ToString() + "' not found");
  const auto new_info = plugins_->Get(*new_plugin);
  if (!new_info || (new_info->state != PluginState::kRegistered && new_info->state != PluginState::kActive)) {
    return Status::PluginRetired("replacement plugin is not active");
  }
  UpgradeReport report;
  report.old_plugin = *old_plugin;
  report.new_plugin = *new_plugin;
  report.operation = operations_.Create(
      "plugin.upgrade_operator", 0, caller,
      JsonValue(JsonObject{{"old", JsonValue(old_key.ToString())}, {"new", JsonValue(new_key.ToString())}}));
  operations_.SetRunning(report.operation);

  // 1. old -> Retiring.
  if (Status s = plugins_->Retire(*old_plugin); !s.ok()) {
    operations_.Fail(report.operation, s);
    return s;
  }
  // 2. snapshot references grouped by session (current topologies only:
  // retired versions finish on their own).
  std::map<SessionId, std::set<std::string>> by_session;
  {
    std::lock_guard lock(sessions_mutex_);
    for (auto& [id, s] : sessions_) {
      const std::shared_ptr<RuntimeTopology> topo = s->current_topology();
      for (const NodeRuntimeRef& n : topo->nodes()) {
        if (n->operator_key() == old_key) by_session[id].insert(n->external_id());
      }
    }
  }
  // 3. one ReplaceNode mutation per session.
  for (const auto& [sid, nodes] : by_session) {
    SessionUpgradeResult r;
    r.session = sid;
    r.nodes.assign(nodes.begin(), nodes.end());
    Session* s = FindSession(sid);
    if (s == nullptr) {
      r.result = Status::NotFound("session vanished");
      report.sessions.push_back(std::move(r));
      continue;
    }
    Mutation m;
    for (const std::string& node : nodes) m.ReplaceNode(node, new_key);
    auto op = s->Apply(m.Build(), caller);
    if (!op.ok()) {
      r.result = op.status();
      report.sessions.push_back(std::move(r));
      continue;
    }
    r.operation = *op;
    // Inline executor: publish now, before the shared queue advances any
    // other session's stream past the point where it could still mutate.
    if (config_.cpu_threads == 0) (void)s->PumpMutations();
    report.sessions.push_back(std::move(r));
  }
  // 4. collect per-session results (wait bounded by drain timeout + slack).
  for (SessionUpgradeResult& r : report.sessions) {
    if (r.operation == 0) continue;
    Session* s = FindSession(r.session);
    const auto timeout = (s != nullptr ? s->options().drain_timeout : config_.default_drain_timeout) +
                         std::chrono::milliseconds(5000);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::optional<OperationRecord> rec;
    for (;;) {
      if (config_.cpu_threads == 0 && s != nullptr) {
        (void)s->PumpMutations();
        (void)executor_->RunPending();
        if (!async_->options().worker_thread) (void)async_->Pump();
        s->Tick();
      }
      rec = operations_.Wait(r.operation, std::chrono::milliseconds(config_.cpu_threads == 0 ? 1 : 50));
      if (rec) break;
      if (std::chrono::steady_clock::now() >= deadline) break;
    }
    if (!rec) {
      r.result = Status::Internal("replace mutation did not finish in time");
    } else if (rec->state == OperationState::kSucceeded) {
      r.result = Status::Ok();
    } else if (rec->state == OperationState::kCancelled) {
      r.result = Status::Cancelled(rec->result.message());
    } else {
      r.result = rec->result;
    }
  }
  // 5/6. all succeeded and references == 0 -> logical unload; otherwise old
  // stays retiring (no rollback of succeeded sessions).
  if (report.all_succeeded()) {
    const Status s = plugins_->LogicalUnload(*old_plugin);
    report.unloaded = s.ok();
    if (s.code() == GE_STATUS_WOULD_BLOCK) {
      std::lock_guard lock(retire_mutex_);
      pending_retires_.push_back(PendingRetire{*old_plugin, 0, false});
    }
  }
  JsonValue detail = report.ToJson();
  if (report.all_succeeded()) {
    operations_.Succeed(report.operation, std::nullopt, std::nullopt, detail);
  } else {
    std::size_t failed = 0;
    for (const SessionUpgradeResult& r : report.sessions) {
      if (!r.result.ok()) ++failed;
    }
    operations_.Fail(report.operation,
                     Status(GE_STATUS_INTERNAL,
                            std::to_string(failed) + " of " + std::to_string(report.sessions.size()) +
                                " sessions failed to upgrade",
                            false),
                     detail);
  }
  PublishSessionEvent(0, "plugin_state", Severity::kInfo, 0, detail);
  return report;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

Result<Session*> Engine::CreateSession(const GraphSpec& spec, CallerContext caller,
                                       OperationId* out_operation) {
  std::lock_guard lock(sessions_mutex_);
  const SessionId sid = next_session_id_++;
  const OperationId op = operations_.Create("session.create", sid, caller,
                                            JsonValue(JsonObject{{"graph", JsonValue(spec.name())}}));
  if (out_operation != nullptr) *out_operation = op;
  operations_.SetRunning(op);
  SessionOptions so;
  so.id = sid;
  so.drain_timeout = config_.default_drain_timeout;
  so.coordinator_thread = config_.cpu_threads != 0;
  so.async_runtime = async_.get();
  if (const auto b = spec.options().extra.GetBool("batching")) so.batching = *b;
  SessionEvents ev;
  ev.on_state_changed = [this, sid](SessionState from, SessionState to) {
    PublishSessionEvent(sid, "session_state", Severity::kInfo, 0,
                        JsonValue(JsonObject{{"from", JsonValue(ToString(from))},
                                             {"to", JsonValue(ToString(to))}}));
  };
  ev.on_node_failed = [this, sid](NodeRuntime& node, const Status& status) {
    PublishSessionEvent(sid, "node_failed", Severity::kError, node.id(),
                        JsonValue(JsonObject{{"node", JsonValue(node.external_id())},
                                             {"code", JsonValue(Status::CodeName(status.code()))},
                                             {"message", JsonValue(status.message())}}));
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
  auto r = Session::Create(spec, factory_, *executor_, operations_, so, std::move(ev));
  if (!r.ok()) {
    operations_.Fail(op, r.status());
    return r.status();
  }
  Session* raw = r->get();
  sessions_[sid] = std::move(*r);
  operations_.Succeed(op, raw->topology_version());
  return raw;
}

Session* Engine::FindSession(SessionId id) const {
  std::lock_guard lock(sessions_mutex_);
  const auto it = sessions_.find(id);
  return it == sessions_.end() ? nullptr : it->second.get();
}

Status Engine::DestroySession(SessionId id) {
  std::unique_ptr<Session> s;
  {
    std::lock_guard lock(sessions_mutex_);
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) return Status::NotFound("session " + std::to_string(id));
    s = std::move(it->second);
    sessions_.erase(it);
  }
  const SessionState st = s->state();
  if (st != SessionState::kStopped && st != SessionState::kCreated) {
    (void)s->Stop(true);
    if (config_.cpu_threads == 0) {
      for (int i = 0; i < 100000 && !s->WaitStopped(std::chrono::milliseconds(0)); ++i) {
        (void)executor_->RunPending();
        if (!async_->options().worker_thread) (void)async_->Pump();
        s->Tick();
      }
    } else {
      (void)s->WaitStopped(std::chrono::milliseconds(5000));
    }
  }
  s.reset();
  CompletePendingUnloads();
  return Status::Ok();
}

void Engine::StopAll(bool fast) {
  std::vector<Session*> sessions;
  {
    std::lock_guard lock(sessions_mutex_);
    for (auto& [id, s] : sessions_) sessions.push_back(s.get());
  }
  for (Session* s : sessions) {
    const SessionState st = s->state();
    if (st == SessionState::kStopped || st == SessionState::kCreated) continue;
    (void)s->Stop(fast);
  }
  for (Session* s : sessions) {
    if (config_.cpu_threads == 0) {
      for (int i = 0; i < 100000 && !s->WaitStopped(std::chrono::milliseconds(0)); ++i) {
        (void)s->PumpMutations();
        (void)executor_->RunPending();
        if (!async_->options().worker_thread) (void)async_->Pump();
        s->Tick();
      }
    } else {
      (void)s->WaitStopped(std::chrono::milliseconds(5000));
    }
  }
}

std::vector<SessionId> Engine::Sessions() const {
  std::lock_guard lock(sessions_mutex_);
  std::vector<SessionId> out;
  for (const auto& [id, s] : sessions_) out.push_back(id);
  return out;
}

}  // namespace ge
