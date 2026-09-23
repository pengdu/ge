#ifndef GE_CPP_ENGINE_H_
#define GE_CPP_ENGINE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <ge/cpp/async_runtime.h>
#include <ge/cpp/audit_log.h>
#include <ge/cpp/event_bus.h>
#include <ge/cpp/graph_spec.h>
#include <ge/cpp/graph_template.h>
#include <ge/cpp/operation.h>
#include <ge/cpp/packet.h>
#include <ge/cpp/plugin_registry.h>
#include <ge/cpp/resource_ledger.h>
#include <ge/cpp/scheduler.h>
#include <ge/cpp/session.h>
#include <ge/cpp/types.h>

namespace ge {

class SessionManager;
class PluginLifecycleService;

struct EngineConfig {
  std::vector<std::filesystem::path> plugin_search_paths;
  std::map<std::string, std::string> host_dependencies;
  std::uint32_t cpu_threads = 4;  // 0 == inline executor (tests)
  std::chrono::milliseconds watchdog_period{10};
  bool watchdog_thread = true;
  EventBus::Options events;
  HostBufferPool::Options buffer_pool;
  // AUD-1: audit ring capacity (observability_config_json `audit_capacity`).
  std::size_t audit_capacity = 4096;
  std::optional<std::string> engine_fingerprint;  // tests only; default GE_BUILD_FINGERPRINT
  std::chrono::milliseconds default_drain_timeout{2000};
  AsyncOptions async;
  std::optional<bool> async_worker_thread;
  // the PluginRegistry by CreateSession/GetCapability; UpgradeOperator stays
  // plugin-only (builtins have no plugin lifecycle; use ReplaceNode).
  std::shared_ptr<OperatorFactory> builtin_operators;
  // ResourceLedger::DefaultCapacities(cpu_threads) at construction;
  // resource_limits_json `resources` overrides per (kind[@device]).
  // `admission: false` disables the ledger entirely (no session ever
  // reserves; RES-1..4 off).
  std::vector<ResourceCapacity> resource_capacities;
  bool resource_admission = true;
  bool reject_unbudgeted_edges = false;

  [[nodiscard]] static Result<EngineConfig> FromJson(const char* plugin_search_paths_json,
                                                     const char* resource_limits_json,
                                                     const char* observability_config_json);
};

struct SessionUpgradeResult {
  SessionId session = 0;
  std::vector<std::string> nodes;  // replaced node ids
  OperationId operation = 0;       // the ReplaceNode mutation
  Status result;
  [[nodiscard]] JsonValue ToJson() const;
};

struct UpgradeReport {
  OperationId operation = 0;
  PluginId old_plugin = 0;
  PluginId new_plugin = 0;
  std::vector<SessionUpgradeResult> sessions;
  bool unloaded = false;  // old plugin logically unloaded (all succeeded, refs == 0)
  [[nodiscard]] bool all_succeeded() const noexcept;
  [[nodiscard]] JsonValue ToJson() const;
};

struct RetirePluginOptions {
  bool request_physical_unload = false;
};

// Assembly root: owns the shared services (pool, ledger, events, plugin
// registry, executor, async runtime) and the two lifecycle services, and
// drives the watchdog. Everything it exposes is either a service accessor or
// a thin forward -- session admission lives in SessionManager, plugin
// lifecycle in PluginLifecycleService, so this file changes when the
// *composition* changes, not when either policy does.
class Engine final {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Engine>> Create(EngineConfig config);
  ~Engine();

  [[nodiscard]] Result<PluginInfo> LoadPlugin(const std::filesystem::path& manifest);
  // Retire -> (refs == 0) logical unload -> optional physical unload. The
  // operation completes when the logical unload happened (or immediately
  // with the physical result); while references remain it stays running
  // and the watchdog finishes it.
  [[nodiscard]] Result<OperationId> RetirePlugin(PluginId id, RetirePluginOptions options = {},
                                                 CallerContext caller = {});
  // cross-session rollback. Blocks until every session's mutation finished
  // (bounded by each session's drain timeout).
  [[nodiscard]] Result<UpgradeReport> UpgradeOperator(const OperatorKey& old_key,
                                                      const OperatorKey& new_key,
                                                      CallerContext caller = {});
  [[nodiscard]] Result<CapabilityDescriptor> GetCapability(const OperatorKey& key) const;

  [[nodiscard]] Result<Session*> CreateSession(const GraphSpec& spec, CallerContext caller = {},
                                               OperationId* out_operation = nullptr);
  // GM-3 batch reuse: validates |tmpl|'s skeleton against this engine's
  // operators once (idempotent; call again after LoadPlugin/UpgradeOperator
  // changed the operator set). Instances created through the overload below
  // skip validation/negotiation and only run per-instance admission -- unless
  // the operator set moved since the last PrevalidateTemplate, in which case
  // they fall back to full validation so a template can never be instantiated
  // against stale capability negotiation.
  [[nodiscard]] Status PrevalidateTemplate(GraphTemplate& tmpl);
  // Operator-set generation of this engine (PluginRegistry). Moves on every
  // plugin load / logical unload.
  [[nodiscard]] std::uint64_t operator_generation() const { return plugins_->operator_generation(); }
  [[nodiscard]] Result<Session*> CreateSession(const GraphTemplate& tmpl, const JsonValue& arguments,
                                               std::string_view instance_name = {}, CallerContext caller = {},
                                               OperationId* out_operation = nullptr);
  [[nodiscard]] Session* FindSession(SessionId id) const;
  // Same lookup, but the returned reference keeps the Session alive: a
  // concurrent DestroySession may remove it from the engine, yet the object
  // survives until the caller drops the pointer (the C API resolves handles
  // this way so a call in flight never races the destructor).
  [[nodiscard]] std::shared_ptr<Session> FindSessionShared(SessionId id) const;
  // Stops (fast) if needed, waits for closure, then frees the session.
  [[nodiscard]] Status DestroySession(SessionId id);
  void StopAll(bool fast);
  [[nodiscard]] std::vector<SessionId> Sessions() const;
  // Live sessions pinned for the caller (a concurrent DestroySession cannot
  // free them mid-walk). Used by RenderPrometheus.
  [[nodiscard]] std::vector<std::shared_ptr<Session>> SessionRefs() const;
  // include/ge/cpp/metrics_export.h.
  [[nodiscard]] std::string RenderPrometheus();

  // completion. Called by the watchdog thread or by the host.
  void Tick();

  [[nodiscard]] PluginRegistry& plugins() noexcept { return *plugins_; }
  [[nodiscard]] OperationRegistry& operations() noexcept { return operations_; }
  // AUD-1/2: every terminal operation, resource rejection and node failure,
  // redacted. Query with AuditFilter; sink for external log shipping.
  [[nodiscard]] AuditLog& audit() noexcept { return audit_; }
  [[nodiscard]] EventBus& events() noexcept { return *events_; }
  [[nodiscard]] ExecutorPool& executor() noexcept { return *executor_; }
  [[nodiscard]] AsyncRuntime& async_runtime() noexcept { return *async_; }
  [[nodiscard]] const std::shared_ptr<HostBufferPool>& buffer_pool() const noexcept { return pool_; }
  // Null when resource_admission == false.
  [[nodiscard]] ResourceLedger* resource_ledger() noexcept { return ledger_.get(); }
  [[nodiscard]] const EngineConfig& config() const noexcept { return config_; }

 private:
  explicit Engine(EngineConfig config);
  [[nodiscard]] Result<Session*> CreateSession(const GraphSpec& spec, CallerContext caller,
                                               OperationId* out_operation,
                                               std::shared_ptr<const ValidatedGraph> prevalidated);
  void WatchdogLoop();
  void PublishSessionEvent(SessionId session, std::string type, Severity severity, NodeId node,
                           JsonValue detail);
  // Session observation hooks are engine policy (events + audit), so the
  // engine supplies them to SessionManager rather than the manager knowing
  // about either.
  [[nodiscard]] SessionEvents MakeSessionEvents(SessionId id, CallerContext caller);

  EngineConfig config_;
  std::shared_ptr<HostBufferPool> pool_;
  std::unique_ptr<ResourceLedger> ledger_;
  std::unique_ptr<EventBus> events_;
  std::unique_ptr<PluginRegistry> plugins_;
  CompositeOperatorFactory factory_;  // builtin_operators, then plugins_
  std::unique_ptr<ExecutorPool> executor_;
  std::unique_ptr<AsyncRuntime> async_;
  OperationRegistry operations_;
  AuditLog audit_;
  // Declared after the services they borrow: destroyed first, so their
  // destructors see a live registry/executor.
  std::unique_ptr<PluginLifecycleService> lifecycle_;
  std::unique_ptr<SessionManager> sessions_;
  std::thread watchdog_;
  std::mutex watchdog_mutex_;
  std::condition_variable watchdog_cv_;
  bool stop_watchdog_ = false;
};

}  // namespace ge

#endif
