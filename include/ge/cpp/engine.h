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
#include <ge/cpp/engine_config.h>
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
#include <ge/cpp/upgrade_report.h>

namespace ge {

class SessionManager;
class PluginLifecycleService;

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
  // Declared after the services they borrow, so they are destroyed first.
  // Their destructors MUST NOT touch any borrowed service: ~Engine resets
  // async_ (and stops the executor) in its body, *before* these members are
  // destroyed, so by then services_.async is already dangling. SessionManager
  // before PluginLifecycleService (construction order): the lifecycle service
  // holds a SessionManager pointer from birth, and the reverse dependency
  // does not exist -- so it is also destroyed first.
  std::unique_ptr<SessionManager> sessions_;
  std::unique_ptr<PluginLifecycleService> lifecycle_;
  std::thread watchdog_;
  std::mutex watchdog_mutex_;
  std::condition_variable watchdog_cv_;
  bool stop_watchdog_ = false;
};

}  // namespace ge

#endif
