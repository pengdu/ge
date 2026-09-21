#ifndef GE_CPP_RESOURCE_LEDGER_H_
#define GE_CPP_RESOURCE_LEDGER_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ge/cpp/capability.h>
#include <ge/cpp/graph_spec.h>
#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge {

// 12 §10.1 resource ledger (RES-1..4, TD-03).
//
// One ledger per Engine. Capacity is per (kind, device); reservations are
// all-or-nothing under one lock; a Lease returns its amounts when destroyed
// or Release()d. Nothing here preempts, prioritises or degrades (SC-7).

enum class ResourceKind : std::uint8_t {
  kCpuThreads,
  kHostMemory,      // bytes
  kPinnedMemory,    // bytes
  kGpuMemory,       // bytes, per device
  kCudaStreams,     // per device
  kNvdecSessions,   // per device
  kNvencSessions,   // per device
  kEdgeBufferBytes  // bytes; 12 §10.3 queue budget
};
inline constexpr std::size_t kResourceKindCount = 8;
[[nodiscard]] std::string_view ToString(ResourceKind kind) noexcept;
// Accepts both the enum spelling ("gpu_memory") and the capability
// `resources.amounts` spelling ("gpu_memory_bytes").
[[nodiscard]] std::optional<ResourceKind> ParseResourceKind(std::string_view name) noexcept;
[[nodiscard]] bool IsPerDevice(ResourceKind kind) noexcept;

struct ResourceAmount {
  ResourceKind kind = ResourceKind::kCpuThreads;
  std::int32_t device_id = -1;  // -1 for host-wide kinds
  std::uint64_t amount = 0;
  friend bool operator==(const ResourceAmount&, const ResourceAmount&) = default;
};

// Sums duplicate (kind, device) entries; returns the merged, sorted list.
[[nodiscard]] std::vector<ResourceAmount> MergeAmounts(std::vector<ResourceAmount> amounts);

struct ResourceCapacity {
  ResourceKind kind = ResourceKind::kCpuThreads;
  std::int32_t device_id = -1;
  std::uint64_t capacity = 0;
};

struct ResourceUsage {
  ResourceKind kind = ResourceKind::kCpuThreads;
  std::int32_t device_id = -1;
  std::uint64_t capacity = 0;
  std::uint64_t reserved = 0;  // sum of live leases
  std::uint64_t peak = 0;      // high-water mark of |reserved|
  [[nodiscard]] std::uint64_t available() const noexcept {
    return capacity > reserved ? capacity - reserved : 0;
  }
  [[nodiscard]] JsonValue ToJson() const;
};

using LeaseId = std::uint64_t;

class ResourceLedger;

// RAII reservation. Move-only; Release() is idempotent.
class ResourceLease final {
 public:
  ResourceLease() = default;
  ResourceLease(ResourceLease&& o) noexcept;
  ResourceLease& operator=(ResourceLease&& o) noexcept;
  ResourceLease(const ResourceLease&) = delete;
  ResourceLease& operator=(const ResourceLease&) = delete;
  ~ResourceLease();

  [[nodiscard]] bool active() const noexcept { return ledger_ != nullptr; }
  [[nodiscard]] LeaseId id() const noexcept { return id_; }
  [[nodiscard]] SessionId owner() const noexcept { return owner_; }
  [[nodiscard]] const std::vector<ResourceAmount>& amounts() const noexcept { return amounts_; }
  // RES-4: after warm-up the node reports what it really took. The delta is
  // reserved (all-or-nothing) or returned; on failure the lease keeps its
  // estimate and the caller decides (12 §10.1 "warm-up fails and rolls back").
  [[nodiscard]] Status Commit(std::vector<ResourceAmount> actual, std::string_view purpose = "warm-up commit");
  void Release() noexcept;

 private:
  friend class ResourceLedger;
  ResourceLease(ResourceLedger* ledger, LeaseId id, SessionId owner, std::vector<ResourceAmount> amounts)
      : ledger_(ledger), id_(id), owner_(owner), amounts_(std::move(amounts)) {}
  ResourceLedger* ledger_ = nullptr;
  LeaseId id_ = 0;
  SessionId owner_ = 0;
  std::vector<ResourceAmount> amounts_;
};

class ResourceLedger final {
 public:
  ResourceLedger() = default;
  // Unlisted (kind, device) pairs are unlimited: an estimate against them
  // is recorded for observability but never rejected. Host-wide kinds use
  // device_id -1; per-device kinds must name a device.
  explicit ResourceLedger(std::vector<ResourceCapacity> capacities);

  void SetCapacity(ResourceKind kind, std::int32_t device_id, std::uint64_t capacity);
  [[nodiscard]] std::optional<std::uint64_t> Capacity(ResourceKind kind, std::int32_t device_id) const;

  // All-or-nothing (12 §10.1). On failure nothing is reserved and the
  // status is RESOURCE_EXHAUSTED with context_json listing every short
  // dimension: {kind, device_id, requested, reserved, capacity, available}
  // plus `retry_after_release: true` (RES-3).
  [[nodiscard]] Result<ResourceLease> Reserve(SessionId owner, std::vector<ResourceAmount> amounts,
                                              std::string_view purpose = {});

  [[nodiscard]] std::vector<ResourceUsage> Usage() const;
  [[nodiscard]] ResourceUsage UsageOf(ResourceKind kind, std::int32_t device_id) const;
  [[nodiscard]] std::size_t live_leases() const;
  [[nodiscard]] JsonValue Snapshot() const;

  // Engine-side defaults: cpu_threads = executor threads (or hardware
  // concurrency when 0), host_memory = physical RAM when the platform can
  // tell, edge_buffer_bytes = half of host_memory. Everything else unlimited.
  [[nodiscard]] static std::vector<ResourceCapacity> DefaultCapacities(std::uint32_t cpu_threads);

 private:
  friend class ResourceLease;
  struct Key {
    ResourceKind kind;
    std::int32_t device_id;
    auto operator<=>(const Key&) const = default;
  };
  struct Account {
    std::optional<std::uint64_t> capacity;  // nullopt: unlimited
    std::uint64_t reserved = 0;
    std::uint64_t peak = 0;
  };
  // |mutex_| held.
  [[nodiscard]] Status TryReserveLocked(const std::vector<ResourceAmount>& amounts, std::string_view purpose);
  void ReleaseLocked(const std::vector<ResourceAmount>& amounts) noexcept;
  void ReleaseLease(LeaseId id, const std::vector<ResourceAmount>& amounts) noexcept;
  [[nodiscard]] Status CommitLease(LeaseId id, std::vector<ResourceAmount>& current,
                                   std::vector<ResourceAmount> actual, std::string_view purpose);

  mutable std::mutex mutex_;
  std::map<Key, Account> accounts_;
  LeaseId next_lease_ = 1;
  std::size_t live_leases_ = 0;
};

// 12 §10.2 estimate aggregation for a candidate topology (or the subset of
// it that is new versus a base). Node estimates come from
// CapabilityDescriptor::resources.amounts (keys parsed by
// ParseResourceKind; a numeric device_id suffix `@N` selects the device,
// otherwise the descriptor's first `cuda` device or -1); edge budgets follow
// 12 §10.3: capacity x max_packet_bytes, where max_packet_bytes is the
// spec's explicit value or one derived from the contract's format
// (video: w*h*bpp for the pixel format; tensor: prod(shape)*dtype size;
// audio/bytes/json/custom: unknown). An edge with no derivable bound and no
// explicit max_packet_bytes is *not* rejected here (the deployment may not
// budget host memory at all); the validator reports it as a warning and the
// engine rejects only when an edge_buffer_bytes capacity is configured.
struct GraphResourceEstimate {
  std::vector<ResourceAmount> amounts;
  std::vector<std::string> unbudgeted_edges;  // no bound could be derived
  [[nodiscard]] JsonValue ToJson() const;
};

[[nodiscard]] std::vector<ResourceAmount> EstimateNodeResources(const CapabilityDescriptor& cap);
[[nodiscard]] std::optional<std::uint64_t> EstimatePacketBytes(const ConnectionContract& contract);
using NodeResourceEstimator =
    std::function<std::vector<ResourceAmount>(const OperatorKey& op, const JsonValue& options)>;
[[nodiscard]] GraphResourceEstimate EstimateGraphResources(
    const GraphSpec& spec, const std::map<std::string, ConnectionContract>& contracts,
    const NodeResourceEstimator& estimate, const std::vector<std::string>& only_nodes = {},
    const std::vector<std::string>& only_edges = {});
// Convenience: static descriptor estimates only.
[[nodiscard]] GraphResourceEstimate EstimateGraphResources(
    const GraphSpec& spec, const std::map<std::string, ConnectionContract>& contracts,
    const std::function<std::optional<CapabilityDescriptor>(const OperatorKey&)>& describe,
    const std::vector<std::string>& only_nodes = {}, const std::vector<std::string>& only_edges = {});

}  // namespace ge

#endif
