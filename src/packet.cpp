#include <ge/cpp/packet.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <new>

#include <ge/cpp/json.h>

namespace ge {

// ---------------------------------------------------------------------------
// Buffer
// ---------------------------------------------------------------------------

ge_buffer_view Buffer::View() const noexcept {
  ge_buffer_view v{};
  v.header.struct_size = sizeof(ge_buffer_view);
  v.header.abi_major = GE_ABI_MAJOR;
  v.data = data;
  v.size = size;
  v.memory_kind = static_cast<ge_memory_kind>(kind);
  v.device_id = device_id;
  return v;
}

BufferRef BufferRef::Retain(Buffer* b) noexcept {
  if (b != nullptr) b->refs.fetch_add(1, std::memory_order_relaxed);
  return BufferRef(b);
}

void BufferRef::Reset() noexcept {
  Buffer* b = std::exchange(buffer_, nullptr);
  if (b == nullptr) return;
  if (b->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    if (b->deleter != nullptr) {
      b->deleter(b, b->deleter_context);
    } else {
      delete b;
    }
  }
}

namespace {

struct ExternalBuffer final : Buffer {
  BufferDeleter user_deleter = nullptr;
  void* user_context = nullptr;
};

void ExternalDeleter(Buffer* buffer, void* /*context*/) {
  auto* eb = static_cast<ExternalBuffer*>(buffer);
  if (eb->user_deleter != nullptr) eb->user_deleter(eb, eb->user_context);
  delete eb;
}

}  // namespace

BufferRef WrapExternalBuffer(void* data, std::size_t size, MemoryKind kind,
                             std::int32_t device_id, BufferDeleter deleter, void* context) {
  auto* b = new ExternalBuffer;
  b->data = data;
  b->size = size;
  b->capacity = size;
  b->kind = kind;
  b->device_id = device_id;
  b->deleter = &ExternalDeleter;
  b->deleter_context = context;  // visible to owners as a payload class tag
  b->user_deleter = deleter;
  b->user_context = context;
  b->refs.store(1, std::memory_order_relaxed);
  return BufferRef::Adopt(b);
}

// ---------------------------------------------------------------------------
// HostBufferPool
// ---------------------------------------------------------------------------

namespace {

struct PooledBuffer final : Buffer {
  std::shared_ptr<HostBufferPool> owner;  // keeps the pool alive while in use
  std::size_t size_class = 0;
};

constexpr std::size_t kMinClass = 256;
constexpr std::size_t kClassCount = 24;  // 256B .. 2GiB

}  // namespace

std::shared_ptr<HostBufferPool> HostBufferPool::Create(Options options) {
  return std::shared_ptr<HostBufferPool>(new HostBufferPool(options));
}

HostBufferPool::HostBufferPool(Options options) : options_(options), free_lists_(kClassCount) {
  if (options_.alignment < alignof(std::max_align_t)) options_.alignment = alignof(std::max_align_t);
  if (!std::has_single_bit(options_.alignment)) options_.alignment = std::bit_ceil(options_.alignment);
}

HostBufferPool::~HostBufferPool() {
  closed_.store(true, std::memory_order_release);
  std::lock_guard lock(mutex_);
  for (auto& list : free_lists_) {
    for (Buffer* b : list) {
      std::free(b->data);
      delete static_cast<PooledBuffer*>(b);
    }
    list.clear();
  }
  cached_bytes_ = 0;
}

std::size_t HostBufferPool::SizeClass(std::size_t size) noexcept {
  std::size_t cls = 0;
  std::size_t cap = kMinClass;
  while (cap < size && cls + 1 < kClassCount) {
    cap <<= 1;
    ++cls;
  }
  return cls;
}

BufferRef HostBufferPool::Allocate(std::size_t size) {
  const std::size_t cls = SizeClass(size);
  const std::size_t cap = kMinClass << cls;
  if (cap < size) return nullptr;  // exceeds the largest class
  PooledBuffer* b = nullptr;
  {
    std::lock_guard lock(mutex_);
    ++allocations_;
    auto& list = free_lists_[cls];
    if (!list.empty()) {
      b = static_cast<PooledBuffer*>(list.back());
      list.pop_back();
      cached_bytes_ -= cap;
      ++pool_hits_;
    }
  }
  if (b == nullptr) {
    void* mem = std::aligned_alloc(options_.alignment,
                                   (cap + options_.alignment - 1) / options_.alignment *
                                       options_.alignment);
    if (mem == nullptr) return nullptr;
    b = new PooledBuffer;
    b->data = mem;
    b->capacity = cap;
    b->kind = MemoryKind::kHost;
    b->device_id = -1;
    b->size_class = cls;
    b->deleter = &HostBufferPool::Recycle;
  }
  b->size = size;
  b->owner = shared_from_this();
  b->deleter_context = this;
  b->refs.store(1, std::memory_order_relaxed);
  live_.fetch_add(1, std::memory_order_relaxed);
  return BufferRef::Adopt(b);
}

void HostBufferPool::Recycle(Buffer* buffer, void* /*context*/) {
  auto* b = static_cast<PooledBuffer*>(buffer);
  std::shared_ptr<HostBufferPool> owner = std::move(b->owner);
  owner->live_.fetch_sub(1, std::memory_order_relaxed);
  const std::size_t cap = kMinClass << b->size_class;
  {
    std::lock_guard lock(owner->mutex_);
    if (!owner->closed_.load(std::memory_order_acquire) &&
        owner->cached_bytes_ + cap <= owner->options_.max_cached_bytes) {
      owner->free_lists_[b->size_class].push_back(b);
      owner->cached_bytes_ += cap;
      return;
    }
  }
  std::free(b->data);
  delete b;
}

HostBufferPool::Stats HostBufferPool::stats() const {
  std::lock_guard lock(mutex_);
  return {allocations_, pool_hits_, live_.load(std::memory_order_relaxed), cached_bytes_};
}

// ---------------------------------------------------------------------------
// TypeTagRegistry
// ---------------------------------------------------------------------------

TypeTagRegistry& TypeTagRegistry::Global() {
  static TypeTagRegistry* instance = new TypeTagRegistry;
  return *instance;
}

TypeTagRegistry::TypeTagRegistry() {
  names_.emplace_back();  // id 0 reserved
  for (const char* tag : {"VideoFrame", "AudioFrame", "Tensor", "Bytes", "Json",
                          "EncodedVideo", "EncodedAudio"}) {
    names_.emplace_back(tag);
  }
  builtin_count_ = names_.size();
}

TypeTagId TypeTagRegistry::Intern(std::string_view tag) {
  if (tag.empty()) return kInvalidTypeTag;
  std::lock_guard lock(mutex_);
  for (std::size_t i = 1; i < names_.size(); ++i) {
    if (names_[i] == tag) return static_cast<TypeTagId>(i);
  }
  names_.emplace_back(tag);
  return static_cast<TypeTagId>(names_.size() - 1);
}

TypeTagId TypeTagRegistry::Find(std::string_view tag) const {
  std::lock_guard lock(mutex_);
  for (std::size_t i = 1; i < names_.size(); ++i) {
    if (names_[i] == tag) return static_cast<TypeTagId>(i);
  }
  return kInvalidTypeTag;
}

std::string_view TypeTagRegistry::Name(TypeTagId id) const {
  std::lock_guard lock(mutex_);
  return id < names_.size() ? std::string_view(names_[id]) : std::string_view{};
}

bool TypeTagRegistry::IsBuiltin(TypeTagId id) const noexcept {
  return id != kInvalidTypeTag && id < builtin_count_;
}

// ---------------------------------------------------------------------------
// Metadata / Packet
// ---------------------------------------------------------------------------

void Metadata::Set(std::string_view key, std::string value) {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                                   [](const auto& e, std::string_view k) { return e.first < k; });
  if (it != entries_.end() && it->first == key) {
    it->second = std::move(value);
  } else {
    entries_.emplace(it, std::string(key), std::move(value));
  }
}

const std::string* Metadata::Get(std::string_view key) const noexcept {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                                   [](const auto& e, std::string_view k) { return e.first < k; });
  return it != entries_.end() && it->first == key ? &it->second : nullptr;
}

std::string Metadata::ToJson() const {
  JsonObject o;
  for (const auto& [k, v] : entries_) o.emplace(k, JsonValue(v));
  return JsonValue(std::move(o)).Serialize();
}

Result<Metadata> Metadata::FromJson(std::string_view json) {
  Metadata m;
  if (json.empty()) return m;
  JsonParseResult parsed = ParseJson(json);
  if (!parsed.ok()) return Status::InvalidArgument("invalid metadata JSON: " + parsed.error);
  if (!parsed.value->is_object()) return Status::InvalidArgument("metadata must be a JSON object");
  for (const auto& [k, v] : parsed.value->as_object()) {
    m.Set(k, v.is_string() ? v.as_string() : v.Serialize());
  }
  return m;
}

const Metadata& Packet::meta() const noexcept {
  static const Metadata kEmpty;
  return metadata ? *metadata : kEmpty;
}

Packet Packet::Eos(TypeTagId tag, TopologyVersion tv, ParameterVersion pv, PacketSeq seq) noexcept {
  Packet p;
  p.header.seq = seq;
  p.header.flags = GE_PACKET_FLAG_EOS;
  p.header.topology_version = tv;
  p.header.parameter_version = pv;
  p.header.type_tag = tag;
  return p;
}

}  // namespace ge
