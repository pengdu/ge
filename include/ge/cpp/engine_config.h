#ifndef GE_CPP_ENGINE_CONFIG_H_
#define GE_CPP_ENGINE_CONFIG_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ge/cpp/async_runtime.h>
#include <ge/cpp/event_bus.h>
#include <ge/cpp/packet.h>
#include <ge/cpp/resource_ledger.h>
#include <ge/cpp/types.h>

namespace ge {

class OperatorFactory;

// Engine construction contract, split from engine.h so components that only
// need the configuration (SessionManager, PluginLifecycleService) do not
// depend on the Engine class itself.
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

}  // namespace ge

#endif  // GE_CPP_ENGINE_CONFIG_H_
