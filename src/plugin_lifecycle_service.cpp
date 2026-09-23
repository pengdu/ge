#include "plugin_lifecycle_service.h"

#include <ge/cpp/audit_log.h>
#include <ge/cpp/scheduler.h>
#include <ge/cpp/operation.h>

#include "session_manager.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <set>
#include <string>

namespace ge {

PluginLifecycleService::PluginLifecycleService(PluginLifecycleServices services, const EngineConfig& config)
    : services_(std::move(services)), config_(config) {
  assert(services_.plugins != nullptr && services_.factory != nullptr &&
         services_.operations != nullptr && services_.audit != nullptr &&
         services_.executor != nullptr && services_.async != nullptr &&
         services_.sessions != nullptr && services_.publish_event != nullptr);
}

PluginLifecycleService::~PluginLifecycleService() = default;

Result<PluginInfo> PluginLifecycleService::Load(const std::filesystem::path& manifest) {
  auto r = services_.plugins->Load(manifest);
  AuditRecord a;
  a.operation = "plugin.load";
  a.result = r.ok() ? GE_STATUS_OK : r.status().code();
  JsonObject d;
  d["manifest"] = JsonValue(manifest.string());
  if (r.ok()) {
    a.target = "plugin:" + std::to_string(r->id);
    d["plugin"] = JsonValue(r->id);
    d["plugin_id"] = JsonValue(r->plugin_id);
    d["state"] = JsonValue(ToString(r->state));
    if (r->state == PluginState::kRejected) {
      a.result = r->rejection.code();
      a.message = r->rejection.message();
    }
  } else {
    a.target = "engine";
    a.message = r.status().message();
  }
  a.digest = JsonValue(std::move(d));
  services_.audit->Append(std::move(a));
  return r;
}

Result<OperationId> PluginLifecycleService::Retire(PluginId id, RetirePluginOptions options,
                                                   CallerContext caller) {
  const auto info = services_.plugins->Get(id);
  if (!info) return Status::NotFound("plugin " + std::to_string(id));
  const OperationId op = services_.operations->Create(
      "plugin.retire", 0, std::move(caller),
      JsonValue(JsonObject{{"plugin", JsonValue(id)},
                           {"physical", JsonValue(options.request_physical_unload)}}));
  if (Status s = services_.plugins->Retire(id); !s.ok()) {
    services_.operations->Fail(op, s);
    return op;
  }
  services_.operations->SetRunning(op);
  {
    std::lock_guard lock(retire_mutex_);
    pending_retires_.push_back(PendingRetire{id, op, options.request_physical_unload});
  }
  CompletePendingUnloads();
  return op;
}

Result<CapabilityDescriptor> PluginLifecycleService::GetCapability(const OperatorKey& key) const {
  // The composite factory, not the registry alone: host builtins answer first
  // and must be visible here exactly as they are to admission.
  const CapabilityDescriptor* cap = services_.factory->Describe(key);
  if (cap == nullptr) return Status::NotFound("operator '" + key.ToString() + "' not found");
  return *cap;
}

// 12 §9.3
Result<UpgradeReport> PluginLifecycleService::UpgradeOperator(const OperatorKey& old_key,
                                                              const OperatorKey& new_key,
                                                              CallerContext caller) {
  if (old_key == new_key) return Status::InvalidArgument("old and new operator keys are identical");
  const auto old_plugin = services_.plugins->Resolve(old_key);
  if (!old_plugin) return Status::NotFound("operator '" + old_key.ToString() + "' not found");
  const auto new_plugin = services_.plugins->Resolve(new_key);
  if (!new_plugin) return Status::NotFound("operator '" + new_key.ToString() + "' not found");
  const auto new_info = services_.plugins->Get(*new_plugin);
  if (!new_info || (new_info->state != PluginState::kRegistered && new_info->state != PluginState::kActive)) {
    return Status::PluginRetired("replacement plugin is not active");
  }
  UpgradeReport report;
  report.old_plugin = *old_plugin;
  report.new_plugin = *new_plugin;
  report.operation = services_.operations->Create(
      "plugin.upgrade_operator", 0, caller,
      JsonValue(JsonObject{{"old", JsonValue(old_key.ToString())}, {"new", JsonValue(new_key.ToString())}}));
  services_.operations->SetRunning(report.operation);

  // 1. old -> Retiring.
  if (Status s = services_.plugins->Retire(*old_plugin); !s.ok()) {
    services_.operations->Fail(report.operation, s);
    return s;
  }
  // 2. snapshot references grouped by session (current topologies only:
  // retired versions finish on their own). One manager pass, then no lock.
  std::map<SessionId, std::set<std::string>> by_session;
  for (const auto& [sid, session] : services_.sessions->RefsWithIds()) {
    const std::shared_ptr<RuntimeTopology> topo = session->current_topology();
    for (const NodeRuntimeRef& n : topo->nodes()) {
      if (n->operator_key() == old_key) by_session[sid].insert(n->external_id());
    }
  }
  // 3. one ReplaceNode mutation per session.
  for (const auto& [sid, nodes] : by_session) {
    SessionUpgradeResult r;
    r.session = sid;
    r.nodes.assign(nodes.begin(), nodes.end());
    std::shared_ptr<Session> s = services_.sessions->FindShared(sid);
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
    std::shared_ptr<Session> s = services_.sessions->FindShared(r.session);
    const auto timeout = (s != nullptr ? s->options().drain_timeout : config_.default_drain_timeout) +
                         std::chrono::milliseconds(5000);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::optional<OperationRecord> rec;
    for (;;) {
      if (config_.cpu_threads == 0 && s != nullptr) {
        (void)s->PumpMutations();
        (void)services_.executor->RunPending();
        if (services_.async != nullptr && !services_.async->options().worker_thread) {
          (void)services_.async->Pump();
        }
        s->Tick();
      }
      rec = services_.operations->Wait(r.operation, std::chrono::milliseconds(config_.cpu_threads == 0 ? 1 : 50));
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
    const Status s = services_.plugins->LogicalUnload(*old_plugin);
    report.unloaded = s.ok();
    if (s.code() == GE_STATUS_WOULD_BLOCK) {
      std::lock_guard lock(retire_mutex_);
      pending_retires_.push_back(PendingRetire{*old_plugin, 0, false});
    }
  }
  JsonValue detail = report.ToJson();
  if (report.all_succeeded()) {
    services_.operations->Succeed(report.operation, std::nullopt, std::nullopt, detail);
  } else {
    std::size_t failed = 0;
    for (const SessionUpgradeResult& r : report.sessions) {
      if (!r.result.ok()) ++failed;
    }
    services_.operations->Fail(report.operation,
                               Status(GE_STATUS_INTERNAL,
                                      std::to_string(failed) + " of " + std::to_string(report.sessions.size()) +
                                          " sessions failed to upgrade",
                                      false),
                               detail);
  }
  services_.publish_event(0, "plugin_state", Severity::kInfo, 0, detail);
  return report;
}

void PluginLifecycleService::CompletePendingUnloads() {
  std::lock_guard pass(unload_pass_mutex_);
  std::vector<PendingRetire> pending;
  {
    std::lock_guard lock(retire_mutex_);
    pending.swap(pending_retires_);
  }
  std::vector<PendingRetire> keep;
  for (const PendingRetire& r : pending) {
    const Status logical = services_.plugins->LogicalUnload(r.plugin);
    if (logical.code() == GE_STATUS_WOULD_BLOCK) {
      keep.push_back(r);
      continue;
    }
    if (!logical.ok()) {
      if (r.operation != 0) services_.operations->Fail(r.operation, logical);
      continue;
    }
    JsonObject d;
    d["logically_unloaded"] = JsonValue(true);
    if (r.physical) {
      const Status physical = services_.plugins->PhysicalUnload(r.plugin);
      d["physically_unloaded"] = JsonValue(physical.ok());
      if (!physical.ok()) {
        // 12 §9.4: logical unload stands; the physical refusal is the result.
        d["physical_unload"] = JsonValue(JsonObject{{"code", JsonValue(Status::CodeName(physical.code()))},
                                                    {"message", JsonValue(physical.message())}});
        if (r.operation != 0) services_.operations->Fail(r.operation, physical, JsonValue(std::move(d)));
        continue;
      }
    }
    if (r.operation != 0) {
      services_.operations->Succeed(r.operation, std::nullopt, std::nullopt, JsonValue(std::move(d)));
    }
  }
  std::lock_guard lock(retire_mutex_);
  pending_retires_.insert(pending_retires_.end(), keep.begin(), keep.end());
}

}  // namespace ge
