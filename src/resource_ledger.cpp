// 12 §10 resource ledger and estimate aggregation (RES-1..4, TD-03).
#include <ge/cpp/resource_ledger.h>

#include <algorithm>
#include <charconv>
#include <thread>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace ge {

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

std::string_view ToString(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::kCpuThreads: return "cpu_threads";
    case ResourceKind::kHostMemory: return "host_memory";
    case ResourceKind::kPinnedMemory: return "pinned_memory";
    case ResourceKind::kGpuMemory: return "gpu_memory";
    case ResourceKind::kCudaStreams: return "cuda_streams";
    case ResourceKind::kNvdecSessions: return "nvdec_sessions";
    case ResourceKind::kNvencSessions: return "nvenc_sessions";
    case ResourceKind::kEdgeBufferBytes: return "edge_buffer_bytes";
  }
  return "?";
}

std::optional<ResourceKind> ParseResourceKind(std::string_view name) noexcept {
  // Strip a `_bytes` suffix so capability spellings map onto the enum.
  if (name.size() > 6 && name.substr(name.size() - 6) == "_bytes" && name != "edge_buffer_bytes") {
    name = name.substr(0, name.size() - 6);
  }
  if (name == "cpu_threads") return ResourceKind::kCpuThreads;
  if (name == "host_memory") return ResourceKind::kHostMemory;
  if (name == "pinned_memory") return ResourceKind::kPinnedMemory;
  if (name == "gpu_memory") return ResourceKind::kGpuMemory;
  if (name == "cuda_streams") return ResourceKind::kCudaStreams;
  if (name == "nvdec_sessions") return ResourceKind::kNvdecSessions;
  if (name == "nvenc_sessions") return ResourceKind::kNvencSessions;
  if (name == "edge_buffer_bytes" || name == "edge_buffer") return ResourceKind::kEdgeBufferBytes;
  return std::nullopt;
}

bool IsPerDevice(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::kGpuMemory:
    case ResourceKind::kCudaStreams:
    case ResourceKind::kNvdecSessions:
    case ResourceKind::kNvencSessions:
      return true;
    default:
      return false;
  }
}

std::vector<ResourceAmount> MergeAmounts(std::vector<ResourceAmount> amounts) {
  std::sort(amounts.begin(), amounts.end(), [](const ResourceAmount& a, const ResourceAmount& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    return a.device_id < b.device_id;
  });
  std::vector<ResourceAmount> out;
  for (const ResourceAmount& a : amounts) {
    if (a.amount == 0) continue;
    if (!out.empty() && out.back().kind == a.kind && out.back().device_id == a.device_id) {
      out.back().amount += a.amount;
    } else {
      out.push_back(a);
    }
  }
  return out;
}

JsonValue ResourceUsage::ToJson() const {
  JsonObject o;
  o.emplace("kind", JsonValue(std::string(ToString(kind))));
  o.emplace("device_id", JsonValue(static_cast<std::int64_t>(device_id)));
  o.emplace("capacity", JsonValue(static_cast<std::int64_t>(capacity)));
  o.emplace("reserved", JsonValue(static_cast<std::int64_t>(reserved)));
  o.emplace("available", JsonValue(static_cast<std::int64_t>(available())));
  o.emplace("peak", JsonValue(static_cast<std::int64_t>(peak)));
  return JsonValue(std::move(o));
}

// ---------------------------------------------------------------------------
// Lease
// ---------------------------------------------------------------------------

ResourceLease::ResourceLease(ResourceLease&& o) noexcept
    : ledger_(o.ledger_), id_(o.id_), owner_(o.owner_), amounts_(std::move(o.amounts_)) {
  o.ledger_ = nullptr;
}

ResourceLease& ResourceLease::operator=(ResourceLease&& o) noexcept {
  if (this != &o) {
    Release();
    ledger_ = o.ledger_;
    id_ = o.id_;
    owner_ = o.owner_;
    amounts_ = std::move(o.amounts_);
    o.ledger_ = nullptr;
  }
  return *this;
}

ResourceLease::~ResourceLease() { Release(); }

void ResourceLease::Release() noexcept {
  if (ledger_ == nullptr) return;
  ledger_->ReleaseLease(id_, amounts_);
  ledger_ = nullptr;
  amounts_.clear();
}

Status ResourceLease::Commit(std::vector<ResourceAmount> actual, std::string_view purpose) {
  if (ledger_ == nullptr) return Status::InvalidArgument("lease is not active");
  return ledger_->CommitLease(id_, amounts_, std::move(actual), purpose);
}

// ---------------------------------------------------------------------------
// Ledger
// ---------------------------------------------------------------------------

ResourceLedger::ResourceLedger(std::vector<ResourceCapacity> capacities) {
  for (const ResourceCapacity& c : capacities) SetCapacity(c.kind, c.device_id, c.capacity);
}

void ResourceLedger::SetCapacity(ResourceKind kind, std::int32_t device_id, std::uint64_t capacity) {
  std::lock_guard lock(mutex_);
  accounts_[Key{kind, device_id}].capacity = capacity;
}

std::optional<std::uint64_t> ResourceLedger::Capacity(ResourceKind kind, std::int32_t device_id) const {
  std::lock_guard lock(mutex_);
  const auto it = accounts_.find(Key{kind, device_id});
  if (it == accounts_.end()) return std::nullopt;
  return it->second.capacity;
}

Status ResourceLedger::TryReserveLocked(const std::vector<ResourceAmount>& amounts, std::string_view purpose) {
  // Pass 1: every dimension must fit; collect all shortfalls (RES-3 wants
  // the caller to see the whole picture, not the first miss).
  JsonArray short_list;
  for (const ResourceAmount& a : amounts) {
    const auto it = accounts_.find(Key{a.kind, a.device_id});
    if (it == accounts_.end() || !it->second.capacity) continue;  // unlimited
    const Account& acc = it->second;
    const std::uint64_t available = *acc.capacity > acc.reserved ? *acc.capacity - acc.reserved : 0;
    if (a.amount <= available) continue;
    JsonObject o;
    o.emplace("kind", JsonValue(std::string(ToString(a.kind))));
    o.emplace("device_id", JsonValue(static_cast<std::int64_t>(a.device_id)));
    o.emplace("requested", JsonValue(static_cast<std::int64_t>(a.amount)));
    o.emplace("reserved", JsonValue(static_cast<std::int64_t>(acc.reserved)));
    o.emplace("capacity", JsonValue(static_cast<std::int64_t>(*acc.capacity)));
    o.emplace("available", JsonValue(static_cast<std::int64_t>(available)));
    short_list.push_back(JsonValue(std::move(o)));
  }
  if (!short_list.empty()) {
    std::string msg = "insufficient resources";
    if (!purpose.empty()) {
      msg += " for ";
      msg += purpose;
    }
    const JsonValue& first = short_list.front();
    msg += ": " + first.as_object().at("kind").as_string() + " requested " +
           std::to_string(first.as_object().at("requested").as_integer()) + ", available " +
           std::to_string(first.as_object().at("available").as_integer());
    JsonObject ctx;
    ctx.emplace("short", JsonValue(std::move(short_list)));
    ctx.emplace("retry_after_release", JsonValue(true));
    if (!purpose.empty()) ctx.emplace("purpose", JsonValue(std::string(purpose)));
    return Status::ResourceExhausted(std::move(msg), JsonValue(std::move(ctx)).Serialize());
  }
  // Pass 2: take everything.
  for (const ResourceAmount& a : amounts) {
    Account& acc = accounts_[Key{a.kind, a.device_id}];
    acc.reserved += a.amount;
    acc.peak = std::max(acc.peak, acc.reserved);
  }
  return Status::Ok();
}

void ResourceLedger::ReleaseLocked(const std::vector<ResourceAmount>& amounts) noexcept {
  for (const ResourceAmount& a : amounts) {
    const auto it = accounts_.find(Key{a.kind, a.device_id});
    if (it == accounts_.end()) continue;
    it->second.reserved = it->second.reserved > a.amount ? it->second.reserved - a.amount : 0;
  }
}

Result<ResourceLease> ResourceLedger::Reserve(SessionId owner, std::vector<ResourceAmount> amounts,
                                              std::string_view purpose) {
  amounts = MergeAmounts(std::move(amounts));
  std::lock_guard lock(mutex_);
  if (Status s = TryReserveLocked(amounts, purpose); !s.ok()) return s;
  const LeaseId id = next_lease_++;
  ++live_leases_;
  return ResourceLease(this, id, owner, std::move(amounts));
}

void ResourceLedger::ReleaseLease(LeaseId /*id*/, const std::vector<ResourceAmount>& amounts) noexcept {
  std::lock_guard lock(mutex_);
  ReleaseLocked(amounts);
  if (live_leases_ > 0) --live_leases_;
}

Status ResourceLedger::CommitLease(LeaseId /*id*/, std::vector<ResourceAmount>& current,
                                   std::vector<ResourceAmount> actual, std::string_view purpose) {
  actual = MergeAmounts(std::move(actual));
  // Split into growth (must be reserved) and shrink (returned).
  std::vector<ResourceAmount> grow, shrink;
  for (const ResourceAmount& a : actual) {
    const auto it = std::find_if(current.begin(), current.end(), [&](const ResourceAmount& c) {
      return c.kind == a.kind && c.device_id == a.device_id;
    });
    const std::uint64_t have = it == current.end() ? 0 : it->amount;
    if (a.amount > have) grow.push_back({a.kind, a.device_id, a.amount - have});
    if (a.amount < have) shrink.push_back({a.kind, a.device_id, have - a.amount});
  }
  for (const ResourceAmount& c : current) {
    const bool named = std::any_of(actual.begin(), actual.end(), [&](const ResourceAmount& a) {
      return a.kind == c.kind && a.device_id == c.device_id;
    });
    if (!named) shrink.push_back(c);  // dimension no longer used
  }
  std::lock_guard lock(mutex_);
  if (Status s = TryReserveLocked(grow, purpose); !s.ok()) return s;
  ReleaseLocked(shrink);
  current = std::move(actual);
  return Status::Ok();
}

std::vector<ResourceUsage> ResourceLedger::Usage() const {
  std::lock_guard lock(mutex_);
  std::vector<ResourceUsage> out;
  for (const auto& [k, acc] : accounts_) {
    out.push_back({k.kind, k.device_id, acc.capacity.value_or(0), acc.reserved, acc.peak});
  }
  return out;
}

ResourceUsage ResourceLedger::UsageOf(ResourceKind kind, std::int32_t device_id) const {
  std::lock_guard lock(mutex_);
  ResourceUsage u{kind, device_id, 0, 0, 0};
  const auto it = accounts_.find(Key{kind, device_id});
  if (it != accounts_.end()) {
    u.capacity = it->second.capacity.value_or(0);
    u.reserved = it->second.reserved;
    u.peak = it->second.peak;
  }
  return u;
}

std::size_t ResourceLedger::live_leases() const {
  std::lock_guard lock(mutex_);
  return live_leases_;
}

JsonValue ResourceLedger::Snapshot() const {
  JsonArray arr;
  for (const ResourceUsage& u : Usage()) arr.push_back(u.ToJson());
  JsonObject o;
  o.emplace("resources", JsonValue(std::move(arr)));
  o.emplace("live_leases", JsonValue(static_cast<std::int64_t>(live_leases())));
  return JsonValue(std::move(o));
}

namespace {

std::optional<std::uint64_t> PhysicalMemoryBytes() {
#if defined(__APPLE__)
  std::uint64_t mem = 0;
  std::size_t len = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0 && mem > 0) return mem;
  return std::nullopt;
#elif defined(__linux__)
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long page = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page > 0) return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
  return std::nullopt;
#else
  return std::nullopt;
#endif
}

}  // namespace

std::vector<ResourceCapacity> ResourceLedger::DefaultCapacities(std::uint32_t cpu_threads) {
  std::vector<ResourceCapacity> out;
  std::uint32_t threads = cpu_threads;
  if (threads == 0) threads = std::max(1U, std::thread::hardware_concurrency());
  out.push_back({ResourceKind::kCpuThreads, -1, threads});
  if (const auto mem = PhysicalMemoryBytes()) {
    out.push_back({ResourceKind::kHostMemory, -1, *mem});
    out.push_back({ResourceKind::kEdgeBufferBytes, -1, *mem / 2});
  }
  return out;
}

// ---------------------------------------------------------------------------
// Estimates (12 §10.2, §10.3)
// ---------------------------------------------------------------------------

namespace {

// Bits per pixel for the formats the media capabilities advertise. Unknown
// formats yield nullopt (the edge stays unbudgeted).
std::optional<std::uint32_t> BitsPerPixel(std::string_view fmt) {
  if (fmt == "yuv420p" || fmt == "nv12" || fmt == "nv21" || fmt == "yuvj420p") return 12;
  if (fmt == "yuv420p10le" || fmt == "p010le" || fmt == "p010") return 24;
  if (fmt == "yuv422p" || fmt == "nv16" || fmt == "uyvy422" || fmt == "yuyv422") return 16;
  if (fmt == "yuv422p10le") return 32;
  if (fmt == "yuv444p" || fmt == "rgb24" || fmt == "bgr24") return 24;
  if (fmt == "yuv444p10le") return 48;
  if (fmt == "rgba" || fmt == "bgra" || fmt == "argb" || fmt == "abgr" || fmt == "rgb0" || fmt == "bgr0") return 32;
  if (fmt == "gray" || fmt == "gray8") return 8;
  if (fmt == "gray16le") return 16;
  return std::nullopt;
}

std::optional<std::uint32_t> DTypeBytes(std::string_view dtype) {
  if (dtype == "float32" || dtype == "int32" || dtype == "uint32") return 4;
  if (dtype == "float16" || dtype == "bfloat16" || dtype == "int16" || dtype == "uint16") return 2;
  if (dtype == "int8" || dtype == "uint8" || dtype == "bool") return 1;
  if (dtype == "float64" || dtype == "int64" || dtype == "uint64") return 8;
  return std::nullopt;
}

}  // namespace

std::optional<std::uint64_t> EstimatePacketBytes(const ConnectionContract& contract) {
  if (contract.video) {
    const SelectedVideoFormat& v = *contract.video;
    const auto bpp = BitsPerPixel(v.pixel_format);
    if (!bpp || !v.width || !v.height) return std::nullopt;
    if (v.width->max <= 0 || v.height->max <= 0) return std::nullopt;
    const std::uint64_t pixels = static_cast<std::uint64_t>(v.width->max) * static_cast<std::uint64_t>(v.height->max);
    return pixels * *bpp / 8;
  }
  if (contract.tensor) {
    const SelectedTensorFormat& t = *contract.tensor;
    const auto elem = DTypeBytes(t.dtype);
    if (!elem || t.shape.empty()) return std::nullopt;
    std::uint64_t count = 1;
    for (const IntRange& d : t.shape) {
      if (d.max <= 0) return std::nullopt;  // dynamic dim: unknown
      count *= static_cast<std::uint64_t>(d.max);
    }
    return count * *elem;
  }
  // Audio (frame size depends on the encoder's frame_size), bytes, json and
  // custom tags: no bound derivable from the contract alone.
  return std::nullopt;
}

std::vector<ResourceAmount> EstimateNodeResources(const CapabilityDescriptor& cap) {
  std::vector<ResourceAmount> out;
  std::int32_t default_device = -1;
  // Per-device kinds without an explicit device go to the descriptor's
  // first device that is not the CPU (there is no device_id on DeviceKind
  // today, so it is 0).
  for (const DeviceKind d : cap.execution.devices) {
    if (d != DeviceKind::kCpu) {
      default_device = 0;
      break;
    }
  }
  for (const auto& [name, value] : cap.resources.amounts) {
    if (value <= 0) continue;
    std::string_view key = name;
    std::int32_t device = -1;
    if (const auto at = key.rfind('@'); at != std::string_view::npos) {
      int parsed = -1;
      const auto tail = key.substr(at + 1);
      if (std::from_chars(tail.data(), tail.data() + tail.size(), parsed).ec == std::errc{}) {
        device = parsed;
        key = key.substr(0, at);
      }
    }
    const auto kind = ParseResourceKind(key);
    if (!kind) continue;  // unknown dimension: ignored, not rejected
    if (IsPerDevice(*kind) && device < 0) device = default_device;
    if (!IsPerDevice(*kind)) device = -1;
    out.push_back({*kind, device, static_cast<std::uint64_t>(value)});
  }
  return MergeAmounts(std::move(out));
}

JsonValue GraphResourceEstimate::ToJson() const {
  JsonArray arr;
  for (const ResourceAmount& a : amounts) {
    JsonObject o;
    o.emplace("kind", JsonValue(std::string(ToString(a.kind))));
    o.emplace("device_id", JsonValue(static_cast<std::int64_t>(a.device_id)));
    o.emplace("amount", JsonValue(static_cast<std::int64_t>(a.amount)));
    arr.push_back(JsonValue(std::move(o)));
  }
  JsonArray un;
  for (const std::string& e : unbudgeted_edges) un.push_back(JsonValue(e));
  JsonObject o;
  o.emplace("amounts", JsonValue(std::move(arr)));
  o.emplace("unbudgeted_edges", JsonValue(std::move(un)));
  return JsonValue(std::move(o));
}

GraphResourceEstimate EstimateGraphResources(
    const GraphSpec& spec, const std::map<std::string, ConnectionContract>& contracts,
    const std::function<std::optional<CapabilityDescriptor>(const OperatorKey&)>& describe,
    const std::vector<std::string>& only_nodes, const std::vector<std::string>& only_edges) {
  return EstimateGraphResources(
      spec, contracts,
      [&describe](const OperatorKey& k, const JsonValue&) -> std::vector<ResourceAmount> {
        const auto cap = describe(k);
        if (!cap) return {};
        return EstimateNodeResources(*cap);
      },
      only_nodes, only_edges);
}

GraphResourceEstimate EstimateGraphResources(const GraphSpec& spec,
                                             const std::map<std::string, ConnectionContract>& contracts,
                                             const NodeResourceEstimator& estimate,
                                             const std::vector<std::string>& only_nodes,
                                             const std::vector<std::string>& only_edges) {
  GraphResourceEstimate est;
  const auto wanted = [](const std::vector<std::string>& only, const std::string& id) {
    return only.empty() || std::find(only.begin(), only.end(), id) != only.end();
  };
  for (const NodeSpec& n : spec.nodes()) {
    if (!wanted(only_nodes, n.id)) continue;
    auto amounts = estimate(n.op, n.options);
    est.amounts.insert(est.amounts.end(), amounts.begin(), amounts.end());
  }
  for (const EdgeSpec& e : spec.edges()) {
    if (!wanted(only_edges, e.id)) continue;
    std::optional<std::uint64_t> per_packet = e.queue.max_packet_bytes;
    if (!per_packet) {
      const auto it = contracts.find(e.id);
      if (it != contracts.end()) per_packet = EstimatePacketBytes(it->second);
    }
    if (!per_packet) {
      est.unbudgeted_edges.push_back(e.id);
      continue;
    }
    est.amounts.push_back({ResourceKind::kEdgeBufferBytes, -1, *per_packet * e.queue.capacity});
  }
  est.amounts = MergeAmounts(std::move(est.amounts));
  return est;
}

}  // namespace ge
