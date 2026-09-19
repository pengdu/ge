#include <ge/cpp/edge_channel.h>

#include <algorithm>

namespace ge {

std::string_view ToString(EdgeState s) noexcept {
  switch (s) {
    case EdgeState::kActive: return "active";
    case EdgeState::kBackpressured: return "backpressured";
    case EdgeState::kDraining: return "draining";
    case EdgeState::kRetired: return "retired";
  }
  return "?";
}

EdgeMetrics::Snapshot EdgeMetrics::Load() const noexcept {
  return {queue_depth.load(std::memory_order_relaxed),
          max_depth.load(std::memory_order_relaxed),
          pushed.load(std::memory_order_relaxed),
          popped.load(std::memory_order_relaxed),
          drop_count.load(std::memory_order_relaxed),
          would_block_count.load(std::memory_order_relaxed),
          cancelled_count.load(std::memory_order_relaxed)};
}

EdgeChannel::EdgeChannel(EdgeId id, std::string external_id, ConnectionContract contract,
                         EdgeConfig config, PortRef from, PortRef to)
    : id_(id),
      external_id_(std::move(external_id)),
      contract_(std::move(contract)),
      config_(config),
      from_(std::move(from)),
      to_(std::move(to)),
      ring_(std::max<std::uint32_t>(config.capacity, 1)) {
  config_.capacity = static_cast<std::uint32_t>(ring_.size());
}

void EdgeChannel::MarkDraining() noexcept {
  EdgeState s = state_.load(std::memory_order_acquire);
  while (s == EdgeState::kActive || s == EdgeState::kBackpressured) {
    if (state_.compare_exchange_weak(s, EdgeState::kDraining, std::memory_order_acq_rel)) return;
  }
}

void EdgeChannel::MarkRetired(bool fast) noexcept {
  if (!fast) {
    state_.store(EdgeState::kRetired, std::memory_order_release);
    return;
  }
  std::lock_guard lock(mutex_);
  state_.store(EdgeState::kRetired, std::memory_order_release);
  for (PacketRef& p : ring_) p.reset();
  head_ = 0;
  size_ = 0;
  metrics_.queue_depth.store(0, std::memory_order_release);
}

void EdgeChannel::BumpDepth(std::uint32_t depth) noexcept {
  metrics_.queue_depth.store(depth, std::memory_order_release);
  std::uint32_t max = metrics_.max_depth.load(std::memory_order_relaxed);
  while (depth > max &&
         !metrics_.max_depth.compare_exchange_weak(max, depth, std::memory_order_relaxed)) {
  }
}

bool EdgeChannel::TailHasEos() const noexcept {
  if (size_ == 0) return false;
  const PacketRef& tail = ring_[(head_ + size_ - 1) % ring_.size()];
  return tail && tail->is_eos();
}

void EdgeChannel::PushLocked(PacketRef packet) {
  ring_[(head_ + size_) % ring_.size()] = std::move(packet);
  ++size_;
  metrics_.pushed.fetch_add(1, std::memory_order_relaxed);
  BumpDepth(size_);
}

PushOutcome EdgeChannel::Push(PacketRef packet) {
  const EdgeState s = state_.load(std::memory_order_acquire);
  if (s == EdgeState::kDraining || s == EdgeState::kRetired) {
    metrics_.cancelled_count.fetch_add(1, std::memory_order_relaxed);
    return PushOutcome::kCancelled;
  }
  const bool eos = packet && packet->is_eos();
  std::lock_guard lock(mutex_);
  // Re-check under the lock: a fast MarkRetired may have cleared the ring
  // between the state load above and lock acquisition.
  if (const EdgeState now = state_.load(std::memory_order_acquire);
      now == EdgeState::kDraining || now == EdgeState::kRetired) {
    metrics_.cancelled_count.fetch_add(1, std::memory_order_relaxed);
    return PushOutcome::kCancelled;
  }
  // Idempotent EOS: a marker already at the tail is not duplicated.
  if (eos && TailHasEos()) return PushOutcome::kAccepted;
  if (size_ < ring_.size()) {
    PushLocked(std::move(packet));
    return PushOutcome::kAccepted;
  }
  if (eos) {
    // 12 §4.4 step 5: never drop EOS; wait for a slot with block semantics.
    metrics_.would_block_count.fetch_add(1, std::memory_order_relaxed);
    EdgeState expected = EdgeState::kActive;
    state_.compare_exchange_strong(expected, EdgeState::kBackpressured, std::memory_order_acq_rel);
    return PushOutcome::kWouldBlock;
  }
  switch (config_.policy) {
    case DropPolicy::kDropNewest:
      metrics_.drop_count.fetch_add(1, std::memory_order_relaxed);
      return PushOutcome::kDroppedNewest;
    case DropPolicy::kDropOldest: {
      // Never evict an EOS marker that is already queued.
      std::uint32_t victim = head_;
      std::uint32_t scanned = 0;
      while (scanned < size_ && ring_[victim] && ring_[victim]->is_eos()) {
        victim = (victim + 1) % static_cast<std::uint32_t>(ring_.size());
        ++scanned;
      }
      if (scanned == size_) {
        metrics_.drop_count.fetch_add(1, std::memory_order_relaxed);
        return PushOutcome::kDroppedNewest;
      }
      if (victim == head_) {
        ring_[head_].reset();
        head_ = (head_ + 1) % static_cast<std::uint32_t>(ring_.size());
      } else {
        // Shift the tail left over the victim to keep FIFO order.
        for (std::uint32_t i = victim; i != (head_ + size_ - 1) % ring_.size();
             i = (i + 1) % static_cast<std::uint32_t>(ring_.size())) {
          ring_[i] = std::move(ring_[(i + 1) % ring_.size()]);
        }
        ring_[(head_ + size_ - 1) % ring_.size()].reset();
      }
      --size_;
      metrics_.drop_count.fetch_add(1, std::memory_order_relaxed);
      PushLocked(std::move(packet));
      return PushOutcome::kAcceptedDroppedOldest;
    }
    case DropPolicy::kBlock: {
      metrics_.would_block_count.fetch_add(1, std::memory_order_relaxed);
      EdgeState expected = EdgeState::kActive;
      state_.compare_exchange_strong(expected, EdgeState::kBackpressured, std::memory_order_acq_rel);
      return PushOutcome::kWouldBlock;
    }
  }
  return PushOutcome::kWouldBlock;
}

PushOutcome EdgeChannel::InjectEos(PacketRef eos) {
  std::lock_guard lock(mutex_);
  if (state_.load(std::memory_order_acquire) == EdgeState::kRetired) {
    return PushOutcome::kCancelled;
  }
  // Idempotent: a queued EOS at the tail means the marker is already there.
  if (TailHasEos()) return PushOutcome::kAccepted;
  if (size_ >= ring_.size()) return PushOutcome::kWouldBlock;
  PushLocked(std::move(eos));
  return PushOutcome::kAccepted;
}

std::optional<PacketRef> EdgeChannel::Pop() {
  std::lock_guard lock(mutex_);
  if (size_ == 0) return std::nullopt;
  PacketRef p = std::move(ring_[head_]);
  ring_[head_].reset();
  head_ = (head_ + 1) % static_cast<std::uint32_t>(ring_.size());
  --size_;
  metrics_.popped.fetch_add(1, std::memory_order_relaxed);
  metrics_.queue_depth.store(size_, std::memory_order_release);
  return p;
}

PacketRef EdgeChannel::Peek() const {
  std::lock_guard lock(mutex_);
  return size_ == 0 ? nullptr : ring_[head_];
}

bool EdgeChannel::ClearBackpressure() noexcept {
  if (full()) return false;
  EdgeState expected = EdgeState::kBackpressured;
  return state_.compare_exchange_strong(expected, EdgeState::kActive, std::memory_order_acq_rel);
}

bool EdgeChannel::MarkBackpressured() noexcept {
  EdgeState expected = EdgeState::kActive;
  if (state_.compare_exchange_strong(expected, EdgeState::kBackpressured,
                                     std::memory_order_acq_rel)) {
    metrics_.would_block_count.fetch_add(1, std::memory_order_relaxed);
  } else if (expected != EdgeState::kBackpressured) {
    return false;  // draining/retired: pushes are cancelled, not blocked
  }
  if (full()) return true;
  (void)ClearBackpressure();  // slot freed meanwhile
  return false;
}

}  // namespace ge
