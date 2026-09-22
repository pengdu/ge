#ifndef GE_CPP_PLUGIN_REGISTRY_H_
#define GE_CPP_PLUGIN_REGISTRY_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ge/c/ge_plugin.h>
#include <ge/cpp/capability.h>
#include <ge/cpp/json.h>
#include <ge/cpp/operator.h>
#include <ge/cpp/plugin_manifest.h>
#include <ge/cpp/plugin_operator.h>
#include <ge/cpp/types.h>

namespace ge {

enum class PluginState : std::uint8_t {
  kDiscovered, kValidating, kLoading, kRegistered, kActive, kRetiring,
  kLogicallyUnloaded, kPhysicallyUnloaded, kRejected
};
[[nodiscard]] std::string_view ToString(PluginState s) noexcept;

struct PluginInfo {
  PluginId id = 0;
  std::string plugin_id;
  std::filesystem::path manifest_path;
  std::filesystem::path library_path;
  std::string build_fingerprint;
  PluginState state = PluginState::kDiscovered;
  std::vector<OperatorKey> operators;
  std::uint32_t references = 0;  // live operator instances
  Status rejection;              // kRejected only
  [[nodiscard]] JsonValue ToJson() const;
};

struct PluginReference {
  PluginId plugin_id = 0;
  OperatorKey key;
  SessionId session_id = 0;
  NodeId node_id = 0;
  TopologyVersion topology_version = 0;
  [[nodiscard]] JsonValue ToJson() const;
};

// PLG-8: every load / register / reference / unload step is auditable.
struct PluginAuditRecord {
  std::int64_t timestamp_ns = 0;
  PluginId plugin_id = 0;
  std::string action;  // load / reject / reference / release / retire / logical_unload / physical_unload
  JsonValue detail = JsonValue(JsonObject{});
  [[nodiscard]] JsonValue ToJson() const;
};

struct PluginRegistryOptions {
  // PLG-1: manifests are only accepted below one of these directories.
  std::vector<std::filesystem::path> search_paths;
  // PLG-3: must equal manifest.build_fingerprint and descriptor.build_fingerprint.
  std::string engine_fingerprint = GE_BUILD_FINGERPRINT;
  std::map<std::string, std::string> host_dependencies;
  // Services handed to every plugin operator.
  HostServices services;
  // when the plugin left no thread behind. Default: accept.
  std::function<bool(const PluginInfo&)> thread_probe;
  std::size_t audit_capacity = 4096;
};

// cross-checks, type@version registration, leases and logical/physical
// unload. Doubles as the OperatorFactory of the engine.
class PluginRegistry final : public OperatorFactory {
 public:
  explicit PluginRegistry(PluginRegistryOptions options);
  ~PluginRegistry() override;

  // descriptor -> consistency -> conflicts -> registered. Any failure leaves
  // the registry unchanged (the rejected attempt is only audited).
  [[nodiscard]] Result<PluginInfo> Load(const std::filesystem::path& manifest_path);
  // PLG-9 dry-run: full chain without registering; the handle is closed.
  [[nodiscard]] Result<PluginInfo> DryRunLoad(const std::filesystem::path& manifest_path) const;

  [[nodiscard]] Status Retire(PluginId id);
  // WOULD_BLOCK (retryable) while references remain.
  [[nodiscard]] Status LogicalUnload(PluginId id);
  // otherwise PLUGIN_PHYSICAL_UNLOAD_UNSAFE and the logical state stays.
  [[nodiscard]] Status PhysicalUnload(PluginId id);

  [[nodiscard]] std::optional<PluginInfo> Get(PluginId id) const;
  [[nodiscard]] std::vector<PluginInfo> List() const;
  [[nodiscard]] std::optional<PluginId> Resolve(const OperatorKey& key) const;
  [[nodiscard]] std::vector<PluginReference> SnapshotReferences(PluginId id) const;
  [[nodiscard]] std::vector<PluginReference> SnapshotReferences(const OperatorKey& key) const;
  [[nodiscard]] std::vector<PluginAuditRecord> Audit() const;

  // OperatorFactory: Describe resolves every key whose plugin still has a
  // registry entry (retiring included, so running graphs validate); Create
  // is refused for retiring/unloaded plugins with PLUGIN_RETIRED.
  [[nodiscard]] const CapabilityDescriptor* Describe(const OperatorKey& key) const override;
  [[nodiscard]] Result<std::unique_ptr<Operator>> Create(const OperatorCreateArgs& args) override;

  std::function<void(const PluginInfo&, PluginState from, PluginState to)> on_state_changed;

 private:
  struct OperatorEntry {
    OperatorKey key;
    std::unique_ptr<CapabilityDescriptor> capability;  // stable address
    const ge_operator_vtable* vtable = nullptr;         // valid while loaded
    std::uint32_t flags = 0;
  };
  struct Plugin;
  struct State;
  struct Lease;
  struct Loaded {
    void* handle = nullptr;
    const ge_plugin_descriptor* descriptor = nullptr;
    PluginManifest manifest;
    std::filesystem::path manifest_path;
    std::filesystem::path library_path;
    std::vector<OperatorEntry> operators;
    std::size_t base_thread_count = 0;
  };

  [[nodiscard]] Result<Loaded> Validate(const std::filesystem::path& manifest_path,
                                        std::string* stage) const;
  [[nodiscard]] Status CheckWhitelist(const std::filesystem::path& manifest_path) const;
  [[nodiscard]] Status CheckDependencies(const PluginManifest& manifest) const;
  [[nodiscard]] Status CrossCheck(const PluginManifest& manifest, const ge_plugin_descriptor& d,
                                  std::vector<OperatorEntry>* out) const;
  void Transition(Plugin& p, PluginState to);  // state_->mutex held
  [[nodiscard]] static PluginInfo InfoOf(const Plugin& p);  // state_->mutex held

  PluginRegistryOptions options_;
  // Shared with every lease so operators outliving the registry (never by
  std::shared_ptr<State> state_;
};

}  // namespace ge

#endif
