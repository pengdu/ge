#ifndef GE_CPP_CAPABILITY_H_
#define GE_CPP_CAPABILITY_H_

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ge/cpp/graph_spec.h>
#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge {

// ---------------------------------------------------------------------------
// CapabilityDescriptor (02 §3.3, 12 §8.1, 14 §5).
// Every port capability is normalised into the constraint dimensions of
// 12 §8.1. Output ports *provide* sets/ranges; input ports *accept* them.
// An absent dimension means "unconstrained".
// ---------------------------------------------------------------------------

enum class PortDirection : std::uint8_t { kInput, kOutput };
enum class PortCardinality : std::uint8_t { kSingle, kMulti };
enum class MemoryKind : std::uint8_t { kHost, kPinned, kCudaDevice, kDmaBuf };
enum class DeviceKind : std::uint8_t { kCpu, kGpu };

[[nodiscard]] std::string_view ToString(MemoryKind kind) noexcept;
[[nodiscard]] std::optional<MemoryKind> ParseMemoryKind(std::string_view);
[[nodiscard]] std::string_view ToString(DeviceKind kind) noexcept;
[[nodiscard]] std::optional<DeviceKind> ParseDeviceKind(std::string_view);

template <typename T>
struct Range {
  T min{};
  T max{};
  [[nodiscard]] bool Contains(T v) const noexcept { return v >= min && v <= max; }
  [[nodiscard]] bool Empty() const noexcept { return max < min; }
  [[nodiscard]] Range Intersect(const Range& o) const noexcept {
    return {min > o.min ? min : o.min, max < o.max ? max : o.max};
  }
  friend bool operator==(const Range&, const Range&) = default;
};
using IntRange = Range<std::int64_t>;
using RationalRange = Range<double>;

// Ordered set: element order is the declaring port's preference (12 §8.1
// "priority: ordered preference list"). Intersection preserves the order of
// the *left* operand.
using FormatSet = std::vector<std::string>;

struct VideoConstraints {
  std::optional<FormatSet> pixel_formats;
  std::optional<IntRange> width;
  std::optional<IntRange> height;
  std::optional<RationalRange> fps;
  std::optional<FormatSet> color_spaces;
  std::optional<FormatSet> color_ranges;
  std::optional<FormatSet> hdr_modes;
  friend bool operator==(const VideoConstraints&, const VideoConstraints&) = default;
};

struct AudioConstraints {
  std::optional<FormatSet> sample_formats;
  std::optional<IntRange> sample_rate;
  std::optional<FormatSet> channel_layouts;
  friend bool operator==(const AudioConstraints&, const AudioConstraints&) = default;
};

struct TensorConstraints {
  std::optional<FormatSet> dtypes;
  std::optional<FormatSet> layouts;
  // Per-dimension range; -1 max means unbounded. Empty => unconstrained.
  std::vector<IntRange> shape;
  friend bool operator==(const TensorConstraints&, const TensorConstraints&) = default;
};

struct MemoryConstraints {
  std::optional<std::vector<MemoryKind>> kinds;
  std::optional<std::vector<std::int32_t>> device_ids;
  bool zero_copy_required = false;
  friend bool operator==(const MemoryConstraints&, const MemoryConstraints&) = default;
};

struct PortCapability {
  std::string name;
  PortDirection direction = PortDirection::kInput;
  std::string type_tag;  // builtin type name or opaque business tag
  bool required = true;  // inputs only
  PortCardinality cardinality = PortCardinality::kSingle;
  bool dynamic_consumers = false;  // outputs: may gain consumers via Mutation
  std::optional<VideoConstraints> video;
  std::optional<AudioConstraints> audio;
  std::optional<TensorConstraints> tensor;
  MemoryConstraints memory;
  std::optional<std::vector<SyncPolicy>> sync;  // inputs: supported policies
  friend bool operator==(const PortCapability&, const PortCapability&) = default;
};

struct ExecutionCapability {
  std::vector<DeviceKind> devices;
  bool stateful = true;
  bool async = false;
  std::uint32_t max_parallelism = 1;
  bool zero_copy = false;
  std::optional<std::uint64_t> max_inference_ms;
  friend bool operator==(const ExecutionCapability&, const ExecutionCapability&) = default;
};

struct ParameterCapability {
  std::vector<std::string> hot_updatable;
  std::vector<std::string> migratable;  // 12 §7.5 ReplaceNode migration
  JsonValue schema = JsonValue(JsonObject{});
  friend bool operator==(const ParameterCapability&, const ParameterCapability&) = default;
};

struct ResourceEstimate {
  std::map<std::string, std::int64_t> amounts;  // e.g. gpu_memory_bytes
  friend bool operator==(const ResourceEstimate&, const ResourceEstimate&) = default;
};

struct EventCapability {
  std::vector<std::string> emits;
  std::vector<std::string> accepts;
  friend bool operator==(const EventCapability&, const EventCapability&) = default;
};

using CapabilityVersion = std::uint64_t;

struct CapabilityDescriptor {
  static constexpr int kSchemaVersion = 1;
  static constexpr std::string_view kSchemaId = "ge.dev/schema/capability/v1";

  OperatorKey op;
  std::string description;
  std::vector<PortCapability> inputs;
  std::vector<PortCapability> outputs;
  ExecutionCapability execution;
  ParameterCapability parameters;
  ResourceEstimate resources;
  EventCapability events;
  // Stable hash of the descriptor content; used by NegotiationCacheKey.
  [[nodiscard]] CapabilityVersion Version() const;

  [[nodiscard]] const PortCapability* FindInput(std::string_view name) const noexcept;
  [[nodiscard]] const PortCapability* FindOutput(std::string_view name) const noexcept;

  [[nodiscard]] static Result<CapabilityDescriptor> ParseJson(std::string_view json);
  [[nodiscard]] static Result<CapabilityDescriptor> ParseJson(const JsonValue& doc);
  [[nodiscard]] JsonValue ToJson() const;
  [[nodiscard]] std::string Serialize() const { return ToJson().Serialize(); }

  friend bool operator==(const CapabilityDescriptor&, const CapabilityDescriptor&) = default;
};

// ---------------------------------------------------------------------------
// ConnectionContract (02 §3.3): the frozen result of a negotiation.
// ---------------------------------------------------------------------------

struct SelectedVideoFormat {
  std::string pixel_format;
  std::optional<IntRange> width;
  std::optional<IntRange> height;
  std::optional<RationalRange> fps;
  std::optional<std::string> color_space;
  std::optional<std::string> color_range;
  std::optional<std::string> hdr_mode;
  friend bool operator==(const SelectedVideoFormat&, const SelectedVideoFormat&) = default;
};
struct SelectedAudioFormat {
  std::string sample_format;
  std::optional<IntRange> sample_rate;
  std::optional<std::string> channel_layout;
  friend bool operator==(const SelectedAudioFormat&, const SelectedAudioFormat&) = default;
};
struct SelectedTensorFormat {
  std::string dtype;
  std::optional<std::string> layout;
  std::vector<IntRange> shape;
  friend bool operator==(const SelectedTensorFormat&, const SelectedTensorFormat&) = default;
};

struct ConnectionContract {
  std::string logical_type;
  std::optional<SelectedVideoFormat> video;
  std::optional<SelectedAudioFormat> audio;
  std::optional<SelectedTensorFormat> tensor;
  MemoryKind memory_kind = MemoryKind::kHost;
  std::int32_t device_id = -1;
  SyncPolicy sync_policy = SyncPolicy::kAny;
  CapabilityVersion source_capability = 0;
  CapabilityVersion target_capability = 0;

  [[nodiscard]] JsonValue ToJson() const;
  [[nodiscard]] std::string Serialize() const { return ToJson().Serialize(); }
  // Data-plane equality: everything a packet on the edge depends on.
  // |source_capability|/|target_capability| only fingerprint who negotiated
  // (the consumer set of a fan-out changes on every add/remove of a branch),
  // so they are excluded -- a kept edge whose data contract is unchanged
  // must keep its channel and queued packets (MUT-3, FAN-5).
  [[nodiscard]] bool SameDataPlane(const ConnectionContract& o) const {
    return logical_type == o.logical_type && video == o.video && audio == o.audio &&
           tensor == o.tensor && memory_kind == o.memory_kind && device_id == o.device_id &&
           sync_policy == o.sync_policy;
  }
  friend bool operator==(const ConnectionContract&, const ConnectionContract&) = default;
};

// ---------------------------------------------------------------------------
// Negotiation (12 §8.2–8.4).
// ---------------------------------------------------------------------------

struct PreferenceSet {
  // Caller preference per dimension, e.g. {"pixel_format": ["NV12","P010"]}.
  std::map<std::string, FormatSet> ordered;
  std::optional<MemoryKind> memory_kind;
  std::optional<std::int32_t> device_id;
  std::optional<SyncPolicy> sync_policy;
  [[nodiscard]] std::uint64_t Hash() const;
  friend bool operator==(const PreferenceSet&, const PreferenceSet&) = default;
};

struct CapabilityConflict {
  std::string dimension;      // "logical_type", "pixel_format", "width", ...
  std::string lhs_json;       // output side candidates / range
  std::string rhs_json;       // input side candidates / range
  std::string suggested_converter;  // e.g. "VideoConvert", "VideoScale"
  std::vector<std::string> consumers;  // fan-out: every input "node.port"
  [[nodiscard]] JsonValue ToJson() const;
};

struct NegotiationCacheKey {
  OperatorKey source_operator;
  CapabilityVersion source_capability = 0;
  OperatorKey target_operator;
  CapabilityVersion target_capability = 0;
  std::string source_port;
  std::string target_port;
  std::uint64_t preferences = 0;
  friend auto operator<=>(const NegotiationCacheKey&, const NegotiationCacheKey&) = default;
};

struct FanoutConsumer {
  std::string node_id;
  OperatorKey op;
  CapabilityVersion capability = 0;
  const PortCapability* port = nullptr;
  std::optional<SyncPolicy> edge_sync;  // EdgeSpec::sync override
};

class CapabilityNegotiator final {
 public:
  // 12 §8.2. Deterministic: same inputs => byte-identical contract.
  [[nodiscard]] Result<ConnectionContract> NegotiateEdge(
      const OperatorKey& source_op, CapabilityVersion source_cap,
      const PortCapability& output, const OperatorKey& target_op,
      CapabilityVersion target_cap, const PortCapability& input,
      std::optional<SyncPolicy> edge_sync, const PreferenceSet& pref);

  // 12 §8.3. Consumers are sorted by (node_id, port) before intersecting so
  // the result is independent of declaration order. If |existing| is set,
  // the new common contract must equal it (Mutation adding a consumer).
  [[nodiscard]] Result<ConnectionContract> NegotiateFanout(
      const OperatorKey& source_op, CapabilityVersion source_cap,
      const PortCapability& output, std::vector<FanoutConsumer> consumers,
      const PreferenceSet& pref,
      const ConnectionContract* existing = nullptr);

  // 12 §8.4.
  void InvalidateOperator(const OperatorKey& op);
  void InvalidateAll();
  [[nodiscard]] std::size_t cache_size() const noexcept { return cache_.size(); }

  // Exposed for tests / diagnostics: the last conflict of a failed call.
  [[nodiscard]] const std::optional<CapabilityConflict>& last_conflict() const noexcept {
    return last_conflict_;
  }

 private:
  Result<ConnectionContract> NegotiateUncached(
      const PortCapability& output, const std::vector<const PortCapability*>& inputs,
      std::optional<SyncPolicy> edge_sync, const PreferenceSet& pref,
      const std::vector<std::string>& consumer_names);

  std::map<NegotiationCacheKey, ConnectionContract> cache_;
  std::optional<CapabilityConflict> last_conflict_;
};

// Builtin type families (12 §8.2 type_compatible). Opaque business tags
// match only by exact equality (CAP-7).
[[nodiscard]] bool IsBuiltinTypeTag(std::string_view tag) noexcept;

}  // namespace ge

#endif
