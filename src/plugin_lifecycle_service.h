#ifndef GE_SRC_PLUGIN_LIFECYCLE_SERVICE_H_
#define GE_SRC_PLUGIN_LIFECYCLE_SERVICE_H_

#include <ge/cpp/engine.h>
#include <ge/cpp/plugin_registry.h>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace ge {

class ExecutorPool;
class AsyncRuntime;
class AuditLog;
class OperationRegistry;
class SessionManager;

// Everything the service borrows from its Engine. Raw pointers: the Engine
// owns all of them and outlives the service (it is a member).
struct PluginLifecycleServices {
  PluginRegistry* plugins = nullptr;
  // Capability lookup goes through the composite factory (builtins first,
  // plugins last), same path admission uses.
  OperatorFactory* factory = nullptr;
  OperationRegistry* operations = nullptr;
  AuditLog* audit = nullptr;
  ExecutorPool* executor = nullptr;
  AsyncRuntime* async = nullptr;
  SessionManager* sessions = nullptr;
  std::function<void(SessionId, std::string, Severity, NodeId, JsonValue)> publish_event;
};

// Plugin load / retire / upgrade and the unload passes behind them. Split out
// of Engine because "what happens when the operator set changes" (12 §9,
// plugin references, pending unloads, cross-session rollback) is a different
// reason to change than "what happens when a session is admitted" -- the two
// shared a class and a watchdog slot before.
//
// Locking: |retire_mutex_| guards the pending list only, and |unload_pass_|
// serialises whole passes so a caller returning from DestroySession or
// UpgradeOperator has seen every unload that was possible at that moment.
// Neither mutex nests with SessionManager::mutex_: the upgrade sweep takes a
// pinned session snapshot and leaves the manager's lock before touching a
// session.
class PluginLifecycleService final {
 public:
  PluginLifecycleService(PluginLifecycleServices services, const EngineConfig& config);
  ~PluginLifecycleService();

  PluginLifecycleService(const PluginLifecycleService&) = delete;
  PluginLifecycleService& operator=(const PluginLifecycleService&) = delete;

  [[nodiscard]] Result<PluginInfo> Load(const std::filesystem::path& manifest);
  [[nodiscard]] Result<OperationId> Retire(PluginId id, RetirePluginOptions options, CallerContext caller);
  [[nodiscard]] Result<UpgradeReport> UpgradeOperator(const OperatorKey& old_key, const OperatorKey& new_key,
                                                      CallerContext caller);
  [[nodiscard]] Result<CapabilityDescriptor> GetCapability(const OperatorKey& key) const;

  // Watchdog pass: finishes retires whose references have since dropped.
  void CompletePendingUnloads();

  // The engine builds this service before SessionManager (the manager's event
  // factory captures the engine); the upgrade sweep needs the manager, so it
  // is plugged in right after construction.
  void set_sessions(SessionManager* sessions) noexcept { services_.sessions = sessions; }

 private:
  struct PendingRetire {
    PluginId plugin = 0;
    OperationId operation = 0;  // 0: bookkeeping only (upgrade rollback tail)
    bool physical = false;
  };

  PluginLifecycleServices services_;
  const EngineConfig& config_;
  std::mutex retire_mutex_;
  std::vector<PendingRetire> pending_retires_;
  std::mutex unload_pass_mutex_;
};

}  // namespace ge

#endif  // GE_SRC_PLUGIN_LIFECYCLE_SERVICE_H_
