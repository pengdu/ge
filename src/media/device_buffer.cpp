#include <ge/media/device_buffer.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>

namespace ge::media {

struct FakeDeviceBufferPool::State {
  std::size_t capacity = 0;
  std::mutex mutex;
  std::uint64_t allocations = 0;
  std::uint64_t live_buffers = 0;
  std::uint64_t live_bytes = 0;
};

namespace {

struct DeviceBuffer final : Buffer {
  std::shared_ptr<FakeDeviceBufferPool::State> state;
};

}  // namespace

std::shared_ptr<FakeDeviceBufferPool> FakeDeviceBufferPool::Create(std::int32_t device_id,
                                                                   std::size_t capacity_bytes) {
  return std::shared_ptr<FakeDeviceBufferPool>(new FakeDeviceBufferPool(device_id, capacity_bytes));
}

FakeDeviceBufferPool::FakeDeviceBufferPool(std::int32_t device_id, std::size_t capacity)
    : state_(std::make_shared<State>()), device_id_(device_id) {
  state_->capacity = capacity;
}

FakeDeviceBufferPool::~FakeDeviceBufferPool() = default;

void FakeDeviceBufferPool::Release(Buffer* buffer, void*) {
  auto* b = static_cast<DeviceBuffer*>(buffer);
  {
    std::lock_guard lock(b->state->mutex);
    --b->state->live_buffers;
    b->state->live_bytes -= b->capacity;
  }
  std::free(b->data);
  delete b;
}

BufferRef FakeDeviceBufferPool::Allocate(std::size_t size) {
  {
    std::lock_guard lock(state_->mutex);
    if (state_->live_bytes + size > state_->capacity) return nullptr;
    ++state_->allocations;
    ++state_->live_buffers;
    state_->live_bytes += size;
  }
  void* mem = std::malloc(size == 0 ? 1 : size);
  if (mem == nullptr) {
    std::lock_guard lock(state_->mutex);
    --state_->live_buffers;
    state_->live_bytes -= size;
    return nullptr;
  }
  auto* b = new DeviceBuffer;
  b->data = mem;
  b->size = size;
  b->capacity = size;
  b->kind = MemoryKind::kCudaDevice;
  b->device_id = device_id_;
  b->deleter = &Release;
  b->deleter_context = nullptr;
  b->state = state_;
  b->refs.store(1, std::memory_order_relaxed);
  return BufferRef::Adopt(b);
}

bool FakeDeviceBufferPool::CopyToHost(const Buffer& device, void* host, std::size_t size) {
  if (device.kind != MemoryKind::kCudaDevice || device.device_id != device_id_) return false;
  if (size > device.size) return false;
  std::memcpy(host, device.data, size);
  return true;
}

bool FakeDeviceBufferPool::CopyFromHost(const void* host, std::size_t size, Buffer* device) {
  if (device == nullptr || device->kind != MemoryKind::kCudaDevice || device->device_id != device_id_) return false;
  if (size > device->capacity) return false;
  std::memcpy(device->data, host, size);
  device->size = size;
  return true;
}

DeviceBufferPool::Stats FakeDeviceBufferPool::stats() const {
  std::lock_guard lock(state_->mutex);
  return Stats{state_->allocations, state_->live_buffers, state_->live_bytes};
}

std::string_view ToString(CodecBackend b) noexcept {
  switch (b) {
    case CodecBackend::kSoftware: return "software";
    case CodecBackend::kVideoToolbox: return "videotoolbox";
    case CodecBackend::kNvidia: return "nvidia";
  }
  return "?";
}

std::optional<CodecBackend> ParseCodecBackend(std::string_view text) noexcept {
  if (text == "software" || text.empty()) return CodecBackend::kSoftware;
  if (text == "videotoolbox") return CodecBackend::kVideoToolbox;
  if (text == "nvidia") return CodecBackend::kNvidia;
  return std::nullopt;
}

}  // namespace ge::media
