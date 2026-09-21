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
#include <ge/cpp/event_bus.h>
#include <ge/cpp/graph_spec.h>
#include <ge/cpp/operation.h>
#include <ge/cpp/packet.h>
#include <ge/cpp/plugin_registry.h>
#include <ge/cpp/scheduler.h>
#include <ge/cpp/session.h>
#include <ge/cpp/types.h>

namespace ge {

// 13 §5.1 ge_engine_config in C++ form.
struct EngineConfig {
  std::vector<std::filesystem::path> plugin_search_paths;
  std::map<std::string, std::string> host_dependencies;
  std::uint32_t cpu_threads = 4;  // 0 == inline executor (tests)
  std::chrono::milliseconds watchdog_period{10};
  bool watchdog_thread = true;
  EventBus::Options events;
  HostBufferPool::Options buffer_pool;
  std::optional<std::string> engine_fingerprint;  // tests only; default GE_BUILD_FINGERPRINT
  std::chrono::milliseconds default_drain_timeout{2000};
  // P5: async runtime (worker thread follows cpu_threads != 0 unless set).
  AsyncOptions async;
  std::optional<bool> async_worker_thread;
  // P6: host-provided builtin operators (e.g. ge_media). Consulted before
  // the PluginRegistry by CreateSession/GetCapability; UpgradeOperator stays
  // plugin-only (builtins have no plugin lifecycle; use ReplaceNode).
  std::shared_ptr<OperatorFactory> builtin_operators;

  // ge_engine_config JSON fields (13 §5.1).
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

// 13 §6.3
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

// 13 §7.3 Engine: PluginRegistry, ExecutorPool, OperationRegistry,
// EventBus, Watchdog and SessionManager (12 §12.4).
class Engine final {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Engine>> Create(EngineConfig config);
  ~Engine();

  // Plugins (12 §9).
  [[nodiscard]] Result<PluginInfo> LoadPlugin(const std::filesystem::path& manifest);
  // Retire -> (refs == 0) logical unload -> optional physical unload. The
  // operation completes when the logical unload happened (or immediately
  // with the physical result); while references remain it stays running
  // and the watchdog finishes it.
  [[nodiscard]] Result<OperationId> RetirePlugin(PluginId id, RetirePluginOptions options = {},
                                                 CallerContext caller = {});
  // 12 §9.3 UpgradeAndRetire: per-session ReplaceNode, aggregated, no
  // cross-session rollback. Blocks until every session's mutation finished
  // (bounded by each session's drain timeout).
  [[nodiscard]] Result<UpgradeReport> UpgradeOperator(const OperatorKey& old_key,
                                                      const OperatorKey& new_key,
                                                      CallerContext caller = {});
  [[nodiscard]] Result<CapabilityDescriptor> GetCapability(const OperatorKey& key) const;

  // Sessions (12 §12.4 SessionManager).
  [[nodiscard]] Result<Session*> CreateSession(const GraphSpec& spec, CallerContext caller = {},
                                               OperationId* out_operation = nullptr);
  [[nodiscard]] Session* FindSession(SessionId id) const;
  // Stops (fast) if needed, waits for closure, then frees the session.
  [[nodiscard]] Status DestroySession(SessionId id);
  void StopAll(bool fast);
  [[nodiscard]] std::vector<SessionId> Sessions() const;

  // Watchdog (13 §7.1): drains deadlines, EOS retries, plugin unload
  // completion. Called by the watchdog thread or by the host.
  void Tick();

  [[nodiscard]] PluginRegistry& plugins() noexcept { return *plugins_; }
  [[nodiscard]] OperationRegistry& operations() noexcept { return operations_; }
  [[nodiscard]] EventBus& events() noexcept { return *events_; }
  [[nodiscard]] ExecutorPool& executor() noexcept { return *executor_; }
  [[nodiscard]] AsyncRuntime& async_runtime() noexcept { return *async_; }
  [[nodiscard]] const std::shared_ptr<HostBufferPool>& buffer_pool() const noexcept { return pool_; }
  [[nodiscard]] const EngineConfig& config() const noexcept { return config_; }

 private:
  explicit Engine(EngineConfig config);
  void WatchdogLoop();
  void PublishSessionEvent(SessionId session, std::string type, Severity severity, NodeId node,
                           JsonValue detail);
  void CompletePendingUnloads();

  struct PendingRetire {
    PluginId plugin = 0;
    OperationId operation = 0;
    bool physical = false;
  };

  EngineConfig config_;
  std::shared_ptr<HostBufferPool> pool_;
  std::unique_ptr<EventBus> events_;
  std::unique_ptr<PluginRegistry> plugins_;
  CompositeOperatorFactory factory_;  // builtin_operators, then plugins_
  std::unique_ptr<ExecutorPool> executor_;
  std::unique_ptr<AsyncRuntime> async_;
  OperationRegistry operations_;
  // shared_ptr, not unique_ptr: Tick()/StopAll() snapshot the sessions and
  // call into them outside sessions_mutex_, so a concurrent DestroySession
  // must not free a session the watchdog is still ticking (TSan, Linux CI).
  mutable std::mutex sessions_mutex_;
  std::map<SessionId, std::shared_ptr<Session>> sessions_;
  SessionId next_session_id_ = 1;
  std::mutex retire_mutex_;
  std::vector<PendingRetire> pending_retires_;
  // Serialises CompletePendingUnloads() between the watchdog and callers
  // (DestroySession / UpgradeOperator) so a caller that returns has seen
  // every unload that was possible at that point, not one the watchdog
  // was still working on.
  std::mutex unload_pass_mutex_;
  std::thread watchdog_;
  std::mutex watchdog_mutex_;
  std::condition_variable watchdog_cv_;
  bool stop_watchdog_ = false;
};

}  // namespace ge

#endif
