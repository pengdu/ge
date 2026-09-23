#include "session_manager.h"

#include <ge/cpp/graph_spec_json.h>
#include <ge/cpp/graph_validator.h>

#include <chrono>
#include <thread>

namespace ge {

namespace {

// Inline executors need the host to advance the machinery; a blocking wait
// would spin forever because nothing else pumps it.
constexpr int kInlineSpinLimit = 100000;
// Threaded engines let the executor/async workers finish the stop; bounded so
// a wedged operator cannot hang Destroy forever.
constexpr auto kThreadedStopWait = std::chrono::milliseconds(5000);
// Last-chance wait for every shared_ptr<Session> holder (watchdog snapshots)
// to drop after the session left the map.
constexpr auto kTeardownWait = std::chrono::seconds(5);

}  // namespace

SessionManager::SessionManager(SessionManagerServices services, const EngineConfig& config,
                               SessionEventsFactory make_events)
    : services_(std::move(services)), config_(config), make_events_(std::move(make_events)) {}

SessionManager::~SessionManager() = default;

Status SessionManager::PrevalidateTemplate(GraphTemplate& tmpl) {
  return tmpl.Prevalidate([this](const OperatorKey& k) { return services_.factory->Describe(k); },
                          services_.operator_generation());
}

Result<Session*> SessionManager::Create(const GraphSpec& spec, CallerContext caller,
                                        OperationId* out_operation,
                                        std::shared_ptr<const ValidatedGraph> prevalidated) {
  std::lock_guard lock(mutex_);
  return CreateLocked(spec, std::move(caller), out_operation, std::move(prevalidated));
}

Result<Session*> SessionManager::CreateFromTemplate(const GraphTemplate& tmpl, const JsonValue& arguments,
                                                   std::string_view instance_name, CallerContext caller,
                                                   OperationId* out_operation) {
  Result<GraphSpec> spec = tmpl.Instantiate(arguments, instance_name);
  if (!spec.ok()) return spec.status();
  // The cached ValidatedGraph encodes the capabilities of the operator set
  // that was loaded when Prevalidate() ran. A plugin loaded/retired since
  // then invalidates it: revalidate here rather than negotiate against a
  // stale descriptor set.
  if (!tmpl.validated_for(services_.operator_generation())) {
    const auto resolver = [this](const OperatorKey& k) { return services_.factory->Describe(k); };
    Result<ValidatedGraph> refreshed = GraphValidator(resolver).Validate(tmpl.skeleton());
    if (!refreshed.ok()) return refreshed.status();
    return Create(*spec, std::move(caller), out_operation,
                  std::make_shared<const ValidatedGraph>(std::move(*refreshed)));
  }
  return Create(*spec, std::move(caller), out_operation, tmpl.validated());
}

Result<Session*> SessionManager::CreateLocked(const GraphSpec& spec, CallerContext caller,
                                              OperationId* out_operation,
                                              std::shared_ptr<const ValidatedGraph> prevalidated) {
  const SessionId sid = next_session_id_++;
  // The full spec is the digest (node options may carry URLs / credentials:
  // AuditLog redacts them, AUD-2).
  const OperationId op = services_.operations->Create(
      "session.create", sid, caller,
      JsonValue(JsonObject{{"graph", JsonValue(spec.name())}, {"spec", GraphSpecParser::ToJson(spec)}}));
  if (out_operation != nullptr) *out_operation = op;
  services_.operations->SetRunning(op);
  SessionOptions so;
  so.id = sid;
  so.drain_timeout = config_.default_drain_timeout;
  so.coordinator_thread = config_.cpu_threads != 0;
  so.async_runtime = services_.async;
  so.resource_ledger = services_.ledger;
  so.reject_unbudgeted_edges = config_.reject_unbudgeted_edges;
  so.prevalidated = std::move(prevalidated);
  if (const auto b = spec.options().extra.GetBool("batching")) so.batching = *b;
  SessionEvents ev = make_events_(sid, caller);
  auto r = Session::Create(spec, *services_.factory, *services_.executor, *services_.operations, so,
                           std::move(ev));
  if (!r.ok()) {
    services_.operations->Fail(op, r.status());
    return r.status();
  }
  Session* raw = r->get();
  sessions_[sid] = std::shared_ptr<Session>(std::move(*r));
  services_.operations->Succeed(op, raw->topology_version());
  return raw;
}

Session* SessionManager::Find(SessionId id) const {
  std::lock_guard lock(mutex_);
  const auto it = sessions_.find(id);
  return it == sessions_.end() ? nullptr : it->second.get();
}

std::shared_ptr<Session> SessionManager::FindShared(SessionId id) const {
  std::lock_guard lock(mutex_);
  const auto it = sessions_.find(id);
  return it == sessions_.end() ? nullptr : it->second;
}

Status SessionManager::Destroy(SessionId id) {
  std::shared_ptr<Session> s;
  {
    std::lock_guard lock(mutex_);
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) return Status::NotFound("session " + std::to_string(id));
    s = std::move(it->second);
    sessions_.erase(it);
  }
  const SessionState st = s->state();
  // A failed session still gets its Stop call (the session.stop operation and
  // audit record come from Session::Stop) but is always safe to drop: the
  // scheduler closed everything when the node failed. Only a stopping/running
  // session can still hold a wedged Process.
  const bool failed = st == SessionState::kFailed;
  bool closed_cleanly = st == SessionState::kStopped || st == SessionState::kCreated;
  if (!closed_cleanly) {
    (void)s->Stop(true);
    if (config_.cpu_threads == 0) {
      for (int i = 0; i < kInlineSpinLimit && !s->WaitStopped(std::chrono::milliseconds(0)); ++i) {
        (void)services_.executor->RunPending();
        if (!services_.async->options().worker_thread) (void)services_.async->Pump();
        s->Tick();
      }
      closed_cleanly = failed || s->WaitStopped(std::chrono::milliseconds(0));
    } else {
      closed_cleanly = failed || s->WaitStopped(kThreadedStopWait);
    }
  }
  if (!closed_cleanly) {
    // Quarantine instead of dropping: a wedged operator's in-flight task is
    // still inside Process, and ~Scheduler waits for it -- destroying here
    // would hang Destroy (the old silent path never returned either). Keep
    // the session alive until it stops on its own (ReclaimStopped); plugin
    // leases stay held until then, so a pending unload keeps waiting too.
    {
      std::lock_guard lock(mutex_);
      quarantined_.push_back(std::move(s));
    }
    if (services_.on_stop_timeout) services_.on_stop_timeout(id);
    return Status::Internal("session " + std::to_string(id) +
                            " did not stop within the bounded wait; quarantined until it closes");
  }
  // The watchdog's Tick() may still hold a snapshot reference; the plugin
  // leases are only released by ~Session, and the caller's follow-up unload
  // pass must observe that, so wait for the last reference to drop.
  std::weak_ptr<Session> weak = s;
  s.reset();
  const auto deadline = std::chrono::steady_clock::now() + kTeardownWait;
  while (!weak.expired() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool references_dropped = weak.expired();
  if (!references_dropped && services_.on_stop_timeout) {
    services_.on_stop_timeout(id);
  }
  if (!references_dropped) {
    return Status::Internal("session " + std::to_string(id) +
                            " references did not drop within " + std::to_string(kTeardownWait.count()) + "s");
  }
  return Status::Ok();
}

void SessionManager::ReclaimStopped() {
  std::vector<std::shared_ptr<Session>> still_running;
  {
    std::lock_guard lock(mutex_);
    still_running.swap(quarantined_);
  }
  std::vector<std::shared_ptr<Session>> wedged;
  for (std::shared_ptr<Session>& s : still_running) {
    if (s->state() == SessionState::kStopped) {
      s.reset();  // stopped: no in-flight task can hold ~Scheduler open
    } else {
      wedged.push_back(std::move(s));
    }
  }
  if (!wedged.empty()) {
    std::lock_guard lock(mutex_);
    quarantined_.insert(quarantined_.end(), wedged.begin(), wedged.end());
  }
}

void SessionManager::StopAll(bool fast) {
  const std::vector<std::shared_ptr<Session>> sessions = Refs();
  for (const auto& s : sessions) {
    const SessionState st = s->state();
    if (st == SessionState::kStopped || st == SessionState::kCreated) continue;
    (void)s->Stop(fast);
  }
  for (const auto& s : sessions) {
    bool closed_cleanly = false;
    if (config_.cpu_threads == 0) {
      for (int i = 0; i < kInlineSpinLimit && !s->WaitStopped(std::chrono::milliseconds(0)); ++i) {
        (void)s->PumpMutations();
        (void)services_.executor->RunPending();
        if (!services_.async->options().worker_thread) (void)services_.async->Pump();
        s->Tick();
      }
      closed_cleanly = s->WaitStopped(std::chrono::milliseconds(0));
    } else {
      closed_cleanly = s->WaitStopped(kThreadedStopWait);
    }
    if (!closed_cleanly && services_.on_stop_timeout) {
      services_.on_stop_timeout(s->id());
    }
  }
}

std::vector<SessionId> SessionManager::Ids() const {
  std::lock_guard lock(mutex_);
  std::vector<SessionId> out;
  for (const auto& [id, s] : sessions_) out.push_back(id);
  return out;
}

std::vector<std::shared_ptr<Session>> SessionManager::Refs() const {
  std::lock_guard lock(mutex_);
  std::vector<std::shared_ptr<Session>> out;
  out.reserve(sessions_.size());
  for (const auto& [id, s] : sessions_) out.push_back(s);
  return out;
}

std::vector<std::pair<SessionId, std::shared_ptr<Session>>> SessionManager::RefsWithIds() const {
  std::lock_guard lock(mutex_);
  std::vector<std::pair<SessionId, std::shared_ptr<Session>>> out;
  out.reserve(sessions_.size());
  for (const auto& [id, s] : sessions_) out.emplace_back(id, s);
  return out;
}

void SessionManager::TickAll() {
  for (const std::shared_ptr<Session>& s : Refs()) s->Tick();
  ReclaimStopped();
}

void SessionManager::Clear() {
  std::lock_guard lock(mutex_);
  sessions_.clear();
  // Quarantined sessions too: their wedged operators never finish, so the
  // references simply drop here at shutdown (the on_stop_timeout events
  // already reported them). ~Session may still block on ~Scheduler's wait --
  // engine shutdown cannot outlive a truly stuck operator.
  quarantined_.clear();
}

}  // namespace ge
