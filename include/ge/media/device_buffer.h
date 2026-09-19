#ifndef GE_MEDIA_DEVICE_BUFFER_H_
#define GE_MEDIA_DEVICE_BUFFER_H_

// P6 task 2: GPU device buffer abstraction. The real CUDA/NVDEC backend is
// out of scope for this host (no NVIDIA GPU); the interface is what the
// media operators and negotiation use, and FakeDeviceBufferPool lets the
// device-memory path (memory_kind=cuda_device negotiation, zero-copy
// hand-off, MemoryCopy suggestions) be exercised on any machine.

#include <cstddef>
#include <cstdint>
#include <memory>

#include <ge/cpp/capability.h>
#include <ge/cpp/packet.h>

namespace ge::media {

class DeviceBufferPool {
 public:
  virtual ~DeviceBufferPool() = default;
  [[nodiscard]] virtual MemoryKind kind() const noexcept = 0;
  [[nodiscard]] virtual std::int32_t device_id() const noexcept = 0;
  // Null BufferRef when the device is out of memory.
  [[nodiscard]] virtual BufferRef Allocate(std::size_t size) = 0;
  // Host <-> device copies; return false when a buffer is of the wrong kind.
  [[nodiscard]] virtual bool CopyToHost(const Buffer& device, void* host, std::size_t size) = 0;
  [[nodiscard]] virtual bool CopyFromHost(const void* host, std::size_t size, Buffer* device) = 0;

  struct Stats {
    std::uint64_t allocations = 0;
    std::uint64_t live_buffers = 0;
    std::uint64_t live_bytes = 0;
  };
  [[nodiscard]] virtual Stats stats() const = 0;
};

// Host-memory implementation that reports kind()==cuda_device. Buffers it
// hands out are tagged with the device kind/id so negotiation and the
// operators treat them exactly like real device memory.
class FakeDeviceBufferPool final : public DeviceBufferPool,
                                   public std::enable_shared_from_this<FakeDeviceBufferPool> {
 public:
  static std::shared_ptr<FakeDeviceBufferPool> Create(std::int32_t device_id = 0,
                                                      std::size_t capacity_bytes = std::size_t{256} << 20);
  ~FakeDeviceBufferPool() override;

  [[nodiscard]] MemoryKind kind() const noexcept override { return MemoryKind::kCudaDevice; }
  [[nodiscard]] std::int32_t device_id() const noexcept override { return device_id_; }
  [[nodiscard]] BufferRef Allocate(std::size_t size) override;
  [[nodiscard]] bool CopyToHost(const Buffer& device, void* host, std::size_t size) override;
  [[nodiscard]] bool CopyFromHost(const void* host, std::size_t size, Buffer* device) override;
  [[nodiscard]] Stats stats() const override;

  struct State;

 private:
  FakeDeviceBufferPool(std::int32_t device_id, std::size_t capacity);
  static void Release(Buffer* buffer, void* context);

  std::shared_ptr<State> state_;
  std::int32_t device_id_;
};

// Well-known codec backends (P6 task: hardware backends stay pluggable).
enum class CodecBackend : std::uint8_t { kSoftware, kVideoToolbox, kNvidia };
[[nodiscard]] std::string_view ToString(CodecBackend b) noexcept;
[[nodiscard]] std::optional<CodecBackend> ParseCodecBackend(std::string_view text) noexcept;

}  // namespace ge::media

#endif
