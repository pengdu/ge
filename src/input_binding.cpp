#include <ge/cpp/input_binding.h>

#include <algorithm>
#include <cstdlib>

namespace ge {

InputBinding::InputBinding(std::vector<InputPortBinding> ports, SyncPolicy policy,
                           AlignedOptions aligned)
    : ports_(std::move(ports)), policy_(policy), aligned_(std::move(aligned)) {}

SyncPolicy InputBinding::policy() const {
  std::lock_guard lock(mutex_);
  return policy_;
}

std::vector<InputPortBinding> InputBinding::ports() const {
  std::lock_guard lock(mutex_);
  return ports_;
}

std::optional<InputPortBinding> InputBinding::Find(std::string_view port) const {
  std::lock_guard lock(mutex_);
  for (const InputPortBinding& b : ports_) {
    if (b.port == port) return b;
  }
  return std::nullopt;
}

void InputBinding::Rebind(std::vector<InputPortBinding> ports, SyncPolicy policy,
                          AlignedOptions aligned) {
  std::lock_guard lock(mutex_);
  for (InputPortBinding& n : ports) {
    const auto old = std::find_if(ports_.begin(), ports_.end(),
                                  [&](const InputPortBinding& o) { return o.port == n.port; });
    if (old == ports_.end()) continue;
    if (old->edge == n.edge) {
      n.predecessors = std::move(old->predecessors);
      continue;
    }
    // Port re-wired: the old edge keeps feeding until its EOS marker --
    // unless that marker was already consumed (port ended) and nothing is
    // queued behind it.
    n.predecessors = std::move(old->predecessors);
    const bool ended = eos_ports_.contains(n.port) && old->edge && old->edge->empty();
    if (old->edge && !ended) n.predecessors.push_back(old->edge);
    if (ended) old->edge->MarkRetired(false);
    // A re-wired port is live again even if the old edge had ended.
    eos_ports_.erase(n.port);
  }
  // Ports that vanished: nobody will consume their edges any more.
  for (InputPortBinding& o : ports_) {
    const bool present = std::any_of(ports.begin(), ports.end(),
                                     [&](const InputPortBinding& b) { return b.port == o.port; });
    if (present) continue;
    if (o.edge) o.edge->MarkRetired(true);
    for (const EdgeChannelRef& p : o.predecessors) p->MarkRetired(true);
  }
  // Ports that vanished drop their EOS state.
  for (auto it = eos_ports_.begin(); it != eos_ports_.end();) {
    const bool present = std::any_of(ports.begin(), ports.end(),
                                     [&](const InputPortBinding& b) { return b.port == *it; });
    it = present ? std::next(it) : eos_ports_.erase(it);
  }
  ports_ = std::move(ports);
  policy_ = policy;
  aligned_ = std::move(aligned);
}

void InputBinding::ForgetPredecessor(const EdgeChannel& edge) {
  std::lock_guard lock(mutex_);
  for (InputPortBinding& b : ports_) {
    std::erase_if(b.predecessors, [&](const EdgeChannelRef& p) { return p.get() == &edge; });
  }
}

std::vector<EdgeChannelRef> InputBinding::Edges() const {
  std::lock_guard lock(mutex_);
  std::vector<EdgeChannelRef> out;
  out.reserve(ports_.size());
  for (const InputPortBinding& b : ports_) {
    if (b.edge) out.push_back(b.edge);
    for (const EdgeChannelRef& p : b.predecessors) out.push_back(p);
  }
  return out;
}

std::set<std::string> InputBinding::eos_ports() const {
  std::lock_guard lock(mutex_);
  return eos_ports_;
}

bool InputBinding::InputEnded() const noexcept {
  std::lock_guard lock(mutex_);
  return InputEndedLocked();
}

bool InputBinding::InputEndedLocked() const noexcept {
  bool any_required = false;
  for (const InputPortBinding& b : ports_) {
    if (!b.required) continue;
    any_required = true;
    if (!eos_ports_.contains(b.port)) return false;
  }
  if (any_required) return true;
  // Only optional ports: ended when all of them are EOS.
  for (const InputPortBinding& b : ports_) {
    if (!eos_ports_.contains(b.port)) return false;
  }
  return !ports_.empty();
}

bool InputBinding::MayBeReady() const noexcept {
  std::lock_guard lock(mutex_);
  for (const InputPortBinding& b : ports_) {
    // Mirrors Active(): a draining predecessor that is momentarily empty
    // blocks the port (the new edge must not overtake it), so data queued
    // behind it does not make the node ready (12 §7.3).
    const EdgeChannel* e = Active(b);
    if (e != nullptr && !e->empty()) return true;
  }
  return false;
}

EdgeChannel* InputBinding::Active(const InputPortBinding& b) noexcept {
  for (const EdgeChannelRef& p : b.predecessors) {
    // A fast-retired predecessor is empty and skipped; a draining one is
    // consumed until its EOS marker.
    if (!p->empty()) return p.get();
    if (p->state() != EdgeState::kRetired) return p.get();
  }
  return b.edge.get();
}

PacketRef InputBinding::Take(InputPortBinding& b) {
  EdgeChannel* e = Active(b);
  if (e == nullptr) return nullptr;
  std::optional<PacketRef> p = e->Pop();
  return p ? std::move(*p) : nullptr;
}

PacketRef InputBinding::SkimHead(InputPortBinding& b, InputBatch* out) {
  for (;;) {
    EdgeChannel* e = Active(b);
    if (e == nullptr) return nullptr;
    const bool predecessor = e != b.edge.get();
    PacketRef head = e->Peek();
    if (!head) {
      if (predecessor && e->state() == EdgeState::kRetired) {
        // Emptied by fast retire: nothing more will come from it.
        std::erase_if(b.predecessors, [&](const EdgeChannelRef& p) { return p.get() == e; });
        continue;
      }
      // Predecessor still draining but momentarily empty: wait for it, the
      // new edge must not overtake old in-flight packets.
      return nullptr;
    }
    if (head->is_eos()) {
      (void)e->Pop();
      if (predecessor) {
        // Drained to its marker: nothing else can arrive (12 §7.7).
        e->MarkRetired(false);
        std::erase_if(b.predecessors, [&](const EdgeChannelRef& p) { return p.get() == e; });
        continue;
      }
      eos_ports_.insert(b.port);
      continue;
    }
    if (head->is_event()) {
      if (PacketRef ev = Take(b)) {
        out->events.push_back(std::move(ev));
        out->event_ports.push_back(b.port);
      }
      continue;
    }
    if (!predecessor && IsEos(b.port)) {
      // Data after EOS on the same port is a producer bug; drop and count.
      (void)e->Pop();
      ++stale_dropped_;
      continue;
    }
    return head;
  }
}

std::optional<InputBatch> InputBinding::TryAcquire() {
  std::lock_guard lock(mutex_);
  switch (policy_) {
    case SyncPolicy::kAny: return AcquireAny();
    case SyncPolicy::kLatest: return AcquireLatest();
    case SyncPolicy::kAligned: return AcquireAligned();
  }
  return std::nullopt;
}

// any: the first required port (declaration order) with data triggers; the
// batch carries that single packet. Optional ports are consulted only when
// no required port has data.
std::optional<InputBatch> InputBinding::AcquireAny() {
  InputBatch batch;
  InputPortBinding* chosen = nullptr;
  // Skim every port first so control packets (EOS/events, retired
  // predecessors) are processed even while another port keeps data flowing.
  std::vector<bool> has_data(ports_.size(), false);
  for (std::size_t i = 0; i < ports_.size(); ++i) has_data[i] = SkimHead(ports_[i], &batch) != nullptr;
  for (const bool required_pass : {true, false}) {
    for (std::size_t i = 0; i < ports_.size(); ++i) {
      InputPortBinding& b = ports_[i];
      if (b.required != required_pass || !has_data[i]) continue;
      chosen = &b;
      break;
    }
    if (chosen != nullptr) break;
  }
  if (chosen != nullptr) {
    if (PacketRef p = Take(*chosen)) {
      batch.ports.push_back(chosen->port);
      batch.packets.push_back(std::move(p));
      return batch;
    }
  }
  if (!batch.events.empty()) return batch;
  return std::nullopt;
}

// latest: consume the trigger packet; every other port drops stale packets
// and supplies its newest one (or none if empty/EOS).
std::optional<InputBatch> InputBinding::AcquireLatest() {
  InputBatch batch;
  InputPortBinding* trigger = nullptr;
  for (InputPortBinding& b : ports_) {
    if (!b.required) continue;
    if (SkimHead(b, &batch)) {
      trigger = &b;
      break;
    }
  }
  if (trigger == nullptr) {
    if (!batch.events.empty()) return batch;
    return std::nullopt;
  }
  PacketRef trigger_packet = Take(*trigger);
  if (!trigger_packet) {
    if (!batch.events.empty()) return batch;
    return std::nullopt;
  }
  batch.ports.push_back(trigger->port);
  batch.packets.push_back(std::move(trigger_packet));
  for (InputPortBinding& b : ports_) {
    if (&b == trigger) continue;
    if (!SkimHead(b, &batch)) continue;
    PacketRef latest;
    for (;;) {
      EdgeChannel* e = Active(b);
      const PacketRef head = e == nullptr ? nullptr : e->Peek();
      if (!head || head->is_eos() || head->is_event()) {
        // Leave control packets for the next acquire.
        break;
      }
      PacketRef taken = Take(b);
      if (!taken) break;
      if (latest) ++stale_dropped_;
      latest = std::move(taken);
    }
    if (latest) {
      batch.ports.push_back(b.port);
      batch.packets.push_back(std::move(latest));
    }
  }
  return batch;
}

// aligned (12 §6.3): target PTS from the reference port head (or the
// earliest required port head); every other port contributes its packet
// closest to the target within ±window, dropping packets older than the
// window. Returns a batch only when every required non-EOS port is served.
std::optional<InputBatch> InputBinding::AcquireAligned() {
  InputBatch batch;
  const auto finish = [&](std::optional<InputBatch> r) -> std::optional<InputBatch> {
    if (r) return r;
    if (!batch.events.empty()) return batch;
    return std::nullopt;
  };
  InputPortBinding* fixed_ref = nullptr;
  if (aligned_.reference_port) {
    for (InputPortBinding& b : ports_) {
      if (b.port == *aligned_.reference_port) fixed_ref = &b;
    }
  }

  // Bounded retry: each iteration either returns or drops one late packet.
  for (;;) {
    InputPortBinding* ref = fixed_ref;
    PacketRef ref_head;
    if (ref != nullptr) {
      ref_head = SkimHead(*ref, &batch);
      if (!ref_head) {
        if (IsEos(ref->port)) {
          // Reference ended: input ends now; leftovers are dropped (12 §4.4).
          for (InputPortBinding& b : ports_) {
            while (b.edge && SkimHead(b, &batch)) {
              if (!Take(b)) break;
              ++late_dropped_;
            }
            eos_ports_.insert(b.port);
          }
        }
        return finish(std::nullopt);
      }
    } else {
      std::int64_t best = 0;
      for (InputPortBinding& b : ports_) {
        if (!b.required) continue;
        PacketRef head = SkimHead(b, &batch);
        if (!head) continue;
        if (ref == nullptr || head->header.pts_ns < best) {
          ref = &b;
          best = head->header.pts_ns;
          ref_head = std::move(head);
        }
      }
      if (!ref_head) return finish(std::nullopt);
    }
    const std::int64_t target = ref_head->header.pts_ns;

    // For every other port: drop heads older than the window; classify.
    std::vector<const InputPortBinding*> matched;
    bool wait = false;
    bool ref_is_late = false;
    for (InputPortBinding& b : ports_) {
      if (&b == ref) continue;
      PacketRef head = SkimHead(b, &batch);
      while (head && head->header.pts_ns < target - aligned_.window_ns) {
        if (!Take(b)) break;
        ++late_dropped_;
        head = SkimHead(b, &batch);
      }
      if (!head) {
        if (b.required && !IsEos(b.port)) wait = true;
        continue;
      }
      if (head->header.pts_ns <= target + aligned_.window_ns) {
        matched.push_back(&b);
      } else if (b.required) {
        // This port has moved past the reference packet: the reference is late.
        ref_is_late = true;
      }
    }
    if (ref_is_late && fixed_ref == nullptr) {
      if (!Take(*ref)) return finish(std::nullopt);
      ++late_dropped_;
      continue;
    }
    if (wait) return finish(std::nullopt);
    // Fixed reference with other ports ahead: serve the reference alone and
    // let the node's compensation policy decide (12 §6.3 rule 3).
    // Deliver in port declaration order regardless of which port was the reference.
    for (InputPortBinding& b : ports_) {
      const bool take = (&b == ref) ||
                        std::find(matched.begin(), matched.end(), &b) != matched.end();
      if (!take) continue;
      PacketRef p = Take(b);
      if (!p) continue;  // retired concurrently
      batch.ports.push_back(b.port);
      batch.packets.push_back(std::move(p));
    }
    if (batch.packets.empty()) return finish(std::nullopt);
    return batch;
  }
}

}  // namespace ge
