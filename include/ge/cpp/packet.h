#ifndef GE_CPP_PACKET_H_
#define GE_CPP_PACKET_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ge/c/ge_abi.h>
#include <ge/cpp/capability.h>
#include <ge/cpp/types.h>

namespace ge {

// ---------------------------------------------------------------------------
// Buffer / BufferRef (12 §2.2): intrusive refcount; fan-out shares payload.
// ---------------------------------------------------------------------------

struct Buffer;
using BufferDeleter = void (*)(Buffer* buffer, void* context);

struct Buffer {
  void* data = nullptr;
  std::size_t size = 0;
  std::size_t capacity = 0;
  MemoryKind kind = MemoryKind::kHost;
  std::int32_t device_id = -1;
  std::atomic<std::uint32_t> refs{0};
  BufferDeleter deleter = nullptr;
  void* deleter_context = nullptr;

  [[nodiscard]] ge_buffer_view View() const noexcept;
};

class BufferRef final {
 public:
  BufferRef() = default;
  BufferRef(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)
  // Adopts one reference (the buffer's refs must already count it).
  static BufferRef Adopt(Buffer* b) noexcept { return BufferRef(b); }
  static BufferRef Retain(Buffer* b) noexcept;

  BufferRef(const BufferRef& o) noexcept : BufferRef(Retain(o.buffer_)) {}
  BufferRef(BufferRef&& o) noexcept : buffer_(std::exchange(o.buffer_, nullptr)) {}
  BufferRef& operator=(BufferRef o) noexcept {
    std::swap(buffer_, o.buffer_);
    return *this;
  }
  ~BufferRef() { Reset(); }

  void Reset() noexcept;
  [[nodiscard]] Buffer* get() const noexcept { return buffer_; }
  [[nodiscard]] Buffer* operator->() const noexcept { return buffer_; }
  [[nodiscard]] explicit operator bool() const noexcept { return buffer_ != nullptr; }
  // Gives the caller one reference; pair with Adopt or ge_buffer_release.
  [[nodiscard]] Buffer* Release() noexcept { return std::exchange(buffer_, nullptr); }
  [[nodiscard]] std::uint32_t use_count() const noexcept {
    return buffer_ ? buffer_->refs.load(std::memory_order_acquire) : 0;
  }
  [[nodiscard]] ge_buffer_handle handle() const noexcept {
    return reinterpret_cast<ge_buffer_handle>(buffer_);
  }
  static BufferRef FromHandle(ge_buffer_handle h) noexcept {
    return Retain(reinterpret_cast<Buffer*>(h));
  }

 private:
  explicit BufferRef(Buffer* b) noexcept : buffer_(b) {}
  Buffer* buffer_ = nullptr;
};

// Wraps caller-owned memory; |deleter(buffer, context)| runs when the last
// ref drops. |context| is also stored in Buffer::deleter_context so a
// producer can recognise its own payload class later.
[[nodiscard]] BufferRef WrapExternalBuffer(void* data, std::size_t size, MemoryKind kind,
                                           std::int32_t device_id, BufferDeleter deleter,
                                           void* context);

// Host memory pool with size classes (15 P2 task 1). Thread-safe.
struct HostBufferPoolOptions {
  std::size_t max_cached_bytes = 64u << 20;
  std::size_t alignment = 64;
};

class HostBufferPool final : public std::enable_shared_from_this<HostBufferPool> {
 public:
  using Options = HostBufferPoolOptions;
  static std::shared_ptr<HostBufferPool> Create(Options options = Options{});
  ~HostBufferPool();

  [[nodiscard]] BufferRef Allocate(std::size_t size);

  struct Stats {
    std::uint64_t allocations = 0;
    std::uint64_t pool_hits = 0;
    std::uint64_t live_buffers = 0;
    std::size_t cached_bytes = 0;
  };
  [[nodiscard]] Stats stats() const;

 private:
  explicit HostBufferPool(Options options);
  static void Recycle(Buffer* buffer, void* context);
  static std::size_t SizeClass(std::size_t size) noexcept;

  Options options_;
  mutable std::mutex mutex_;
  std::vector<std::vector<Buffer*>> free_lists_;  // by size class
  std::size_t cached_bytes_ = 0;
  std::uint64_t allocations_ = 0;
  std::uint64_t pool_hits_ = 0;
  std::atomic<std::uint64_t> live_{0};
  std::atomic<bool> closed_{false};
};

// ---------------------------------------------------------------------------
// TypeTag registry (12 §2.2): builtin tags are pre-registered; opaque business
// tags are interned on first use.
// ---------------------------------------------------------------------------

using TypeTagId = std::uint32_t;
inline constexpr TypeTagId kInvalidTypeTag = 0;

class TypeTagRegistry final {
 public:
  static TypeTagRegistry& Global();
  [[nodiscard]] TypeTagId Intern(std::string_view tag);
  [[nodiscard]] TypeTagId Find(std::string_view tag) const;
  [[nodiscard]] std::string_view Name(TypeTagId id) const;
  [[nodiscard]] bool IsBuiltin(TypeTagId id) const noexcept;

  TypeTagRegistry();

 private:
  mutable std::mutex mutex_;
  std::deque<std::string> names_;  // stable addresses: Name() views stay valid
  std::size_t builtin_count_ = 0;
};

// ---------------------------------------------------------------------------
// Packet (12 §2.2). Small-object metadata: sorted vector of key/value strings.
// ---------------------------------------------------------------------------

struct PacketHeader {
  PacketSeq seq = 0;
  std::int64_t pts_ns = 0;
  std::int64_t dts_ns = 0;
  std::uint32_t flags = 0;
  TopologyVersion topology_version = 0;
  ParameterVersion parameter_version = 0;
  TypeTagId type_tag = kInvalidTypeTag;
};

// Well-known metadata keys (12 §2.2).
namespace metadata_keys {
inline constexpr std::string_view kFormat = "format";
inline constexpr std::string_view kError = "error";
inline constexpr std::string_view kDropped = "dropped";
inline constexpr std::string_view kTimeout = "timeout";
}  // namespace metadata_keys

class Metadata final {
 public:
  void Set(std::string_view key, std::string value);
  [[nodiscard]] const std::string* Get(std::string_view key) const noexcept;
  [[nodiscard]] bool Has(std::string_view key) const noexcept { return Get(key) != nullptr; }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& entries() const noexcept {
    return entries_;
  }
  [[nodiscard]] std::string ToJson() const;
  [[nodiscard]] static Result<Metadata> FromJson(std::string_view json);
  friend bool operator==(const Metadata&, const Metadata&) = default;

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

struct Packet {
  PacketHeader header;
  std::shared_ptr<const Metadata> metadata;  // null == empty
  BufferRef payload;
  // OBS-1 end-to-end latency (12 §12.1 SessionMetrics::end_to_end): engine
  // internal, steady-clock ns of the source emit this packet descends from.
  // Stamped by the router when a source emits, inherited from the oldest
  // input of the invocation that produced this packet, read when a sink
  // consumes it. 0 == unknown (not crossing the C ABI; plugins never see it).
  std::int64_t ingress_ns = 0;

  [[nodiscard]] bool is_eos() const noexcept { return header.flags & GE_PACKET_FLAG_EOS; }
  [[nodiscard]] bool is_event() const noexcept { return header.flags & GE_PACKET_FLAG_EVENT; }
  [[nodiscard]] bool is_dropped() const noexcept { return header.flags & GE_PACKET_FLAG_DROPPED; }
  [[nodiscard]] const Metadata& meta() const noexcept;

  [[nodiscard]] static Packet Eos(TypeTagId tag, TopologyVersion tv, ParameterVersion pv,
                                  PacketSeq seq = 0) noexcept;
};

using PacketRef = std::shared_ptr<const Packet>;

}  // namespace ge

#endif
