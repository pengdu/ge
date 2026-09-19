#include <ge/cpp/node_runtime.h>

#include <algorithm>
#include <limits>

namespace ge {

std::string_view ToString(NodeState s) noexcept {
  switch (s) {
    case NodeState::kCreated: return "created";
    case NodeState::kOpening: return "opening";
    case NodeState::kReady: return "ready";
    case NodeState::kRunning: return "running";
    case NodeState::kAsyncPending: return "async_pending";
    case NodeState::kDraining: return "draining";
    case NodeState::kClosing: return "closing";
    case NodeState::kClosed: return "closed";
    case NodeState::kFailed: return "failed";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// ParameterStore
// ---------------------------------------------------------------------------

ParameterStore::ParameterStore(JsonValue initial, std::size_t history_capacity)
    : history_capacity_(history_capacity) {
  auto snap = std::make_shared<ParameterSnapshot>();
  snap->version = 1;
  snap->values = initial.is_object() ? std::move(initial) : JsonValue(JsonObject{});
  current_.store(std::move(snap));
}

Result<ParameterVersion> ParameterStore::Submit(ParameterUpdateRequest request,
                                                const std::vector<std::string>& hot_updatable) {
  if (!request.values.is_object()) {
    return Status::InvalidArgument("parameter update must be a JSON object");
  }
  for (const auto& [key, _] : request.values.as_object()) {
    if (std::find(hot_updatable.begin(), hot_updatable.end(), key) == hot_updatable.end()) {
      return Status::ParameterUnsupported("parameter '" + key +
                                          "' is not hot-updatable; use a Mutation");
    }
  }
  std::lock_guard lock(mutex_);
  last_hot_updatable_ = hot_updatable;
  if (pending_) {
    queued_.push_back(std::move(request));  // PAR-5 serial semantics
    return pending_->version + static_cast<ParameterVersion>(queued_.size());
  }
  const auto base = Current();
  auto next = std::make_shared<ParameterSnapshot>();
  next->version = base->version + 1;
  next->values = base->values;
  JsonObject merged = next->values.as_object();
  for (const auto& [key, value] : request.values.as_object()) merged[key] = value;
  next->values = JsonValue(std::move(merged));
  pending_ = std::move(next);
  return pending_->version;
}

void ParameterStore::PromoteQueued() {
  // Caller holds mutex_ and pending_ is empty.
  while (!queued_.empty() && !pending_) {
    ParameterUpdateRequest req = std::move(queued_.front());
    queued_.pop_front();
    const auto base = Current();
    auto next = std::make_shared<ParameterSnapshot>();
    next->version = base->version + 1;
    JsonObject merged = base->values.as_object();
    bool ok = true;
    for (const auto& [key, value] : req.values.as_object()) {
      if (std::find(last_hot_updatable_.begin(), last_hot_updatable_.end(), key) ==
          last_hot_updatable_.end()) {
        ok = false;
        break;
      }
      merged[key] = value;
    }
    if (!ok) continue;
    next->values = JsonValue(std::move(merged));
    pending_ = std::move(next);
  }
}

std::optional<ParameterVersion> ParameterStore::SwitchAtPacketBoundary(PacketSeq seq) {
  std::lock_guard lock(mutex_);
  if (!pending_) return std::nullopt;
  pending_->effective_after_seq = seq;
  std::shared_ptr<const ParameterSnapshot> snap = std::move(pending_);
  pending_.reset();
  current_.store(snap);
  history_.push_back({snap->version, seq, snap->values.Serialize()});
  while (history_.size() > history_capacity_) history_.pop_front();
  PromoteQueued();
  return snap->version;
}

bool ParameterStore::has_pending() const {
  std::lock_guard lock(mutex_);
  return pending_ != nullptr;
}

std::vector<ParameterAuditRecord> ParameterStore::history() const {
  std::lock_guard lock(mutex_);
  return {history_.begin(), history_.end()};
}

void ParameterStore::RejectPending() {
  std::lock_guard lock(mutex_);
  pending_.reset();
  PromoteQueued();
}

// ---------------------------------------------------------------------------
// NodeRuntime
// ---------------------------------------------------------------------------

NodeRuntime::NodeRuntime(NodeId id, std::string external_id, OperatorKey key,
                         const CapabilityDescriptor& capability, std::unique_ptr<Operator> op,
                         JsonValue options, std::uint32_t parallelism)
    : id_(id),
      external_id_(std::move(external_id)),
      key_(std::move(key)),
      capability_(capability),
      op_(std::move(op)),
      options_(std::move(options)),
      max_parallelism_((capability.execution.stateful || capability.execution.async)
                           ? 1u
                           : std::clamp(parallelism, 1u,
                                        std::max(1u, capability.execution.max_parallelism))),
      parameters_(options_),
      available_slots_(max_parallelism_) {
  if (const auto cap = options_.GetInteger("async_max_in_flight"); cap && *cap >= 1) {
    async_max_in_flight_ = static_cast<std::uint32_t>(std::min<std::int64_t>(*cap, 4096));
  }
}

Status NodeRuntime::Open(const OpenRequest& request) {
  NodeState expected = NodeState::kCreated;
  if (!state_.compare_exchange_strong(expected, NodeState::kOpening, std::memory_order_acq_rel)) {
    return Status::InvalidArgument("node '" + external_id_ + "' cannot open from state " +
                                   std::string(ToString(expected)));
  }
  Status s = op_->Open(request);
  if (!s.ok()) {
    failure_ = Status::NodeWarmupFailed("node '" + external_id_ + "' open failed: " + s.message());
    state_.store(NodeState::kFailed, std::memory_order_release);
    return failure_;
  }
  state_.store(NodeState::kReady, std::memory_order_release);
  return Status::Ok();
}

Status NodeRuntime::Close(const CloseRequest& request) {
  const NodeState s = state_.load(std::memory_order_acquire);
  if (s == NodeState::kClosed) return Status::Ok();
  if (s == NodeState::kCreated) {
    state_.store(NodeState::kClosed, std::memory_order_release);
    return Status::Ok();
  }
  state_.store(NodeState::kClosing, std::memory_order_release);
  Status r = op_->Close(request);
  state_.store(NodeState::kClosed, std::memory_order_release);
  return r;
}

void NodeRuntime::Fail(Status status) {
  failure_ = std::move(status);
  metrics_.errors.fetch_add(1, std::memory_order_relaxed);
  state_.store(NodeState::kFailed, std::memory_order_release);
}

bool NodeRuntime::TrySchedule() noexcept {
  const NodeState s = state_.load(std::memory_order_acquire);
  if (s != NodeState::kReady && s != NodeState::kRunning) return false;
  if (held_.load(std::memory_order_seq_cst)) return false;
  std::uint32_t slots = available_slots_.load(std::memory_order_seq_cst);
  while (slots > 0) {
    if (available_slots_.compare_exchange_weak(slots, slots - 1, std::memory_order_seq_cst)) {
      // Re-check after reserving (Dekker with Hold()/quiescent()): a Hold()
      // that raced with the reservation must observe the slot as taken or we
      // must observe the hold.
      if (held_.load(std::memory_order_seq_cst)) {
        available_slots_.fetch_add(1, std::memory_order_seq_cst);
        return false;
      }
      return true;
    }
  }
  return false;
}

void NodeRuntime::CancelSchedule() noexcept {
  available_slots_.fetch_add(1, std::memory_order_acq_rel);
}

void NodeRuntime::BeginInvoke() noexcept {
  in_flight_.fetch_add(1, std::memory_order_acq_rel);
  NodeState expected = NodeState::kReady;
  state_.compare_exchange_strong(expected, NodeState::kRunning, std::memory_order_acq_rel);
  metrics_.invocations.fetch_add(1, std::memory_order_relaxed);
}

void NodeRuntime::EndInvoke() noexcept {
  const std::uint32_t left = in_flight_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  available_slots_.fetch_add(1, std::memory_order_acq_rel);
  if (left == 0) {
    NodeState expected = NodeState::kRunning;
    state_.compare_exchange_strong(expected, NodeState::kReady, std::memory_order_acq_rel);
  }
}

bool NodeRuntime::TryEnterDraining() noexcept {
  if (in_flight_.load(std::memory_order_acquire) != 0) return false;
  if (async_pending_.load(std::memory_order_acquire) != 0) return false;
  // Every slot must be free, not just in_flight == 0: a sibling task that
  // already popped a batch but has not reached BeginInvoke yet would emit
  // after our EOS (12 §4.4 step 3 "no invocation may follow the flush").
  // Callers return their own slot (CancelSchedule/EndInvoke) before asking.
  if (available_slots_.load(std::memory_order_seq_cst) != max_parallelism_) return false;
  NodeState expected = NodeState::kReady;
  // Exactly one caller wins the ready -> draining transition and owns the
  // flush/EOS/close sequence (12 §4.4 step 3).
  if (!state_.compare_exchange_strong(expected, NodeState::kDraining, std::memory_order_seq_cst)) return false;
  // Dekker with TrySchedule: a reservation that raced with the CAS either
  // saw draining (and fails) or took a slot we now observe; undo then.
  if (available_slots_.load(std::memory_order_seq_cst) != max_parallelism_) {
    NodeState draining = NodeState::kDraining;
    (void)state_.compare_exchange_strong(draining, NodeState::kReady, std::memory_order_seq_cst);
    return false;
  }
  return true;
}

void NodeRuntime::BeginAsync() noexcept {
  async_pending_.fetch_add(1, std::memory_order_seq_cst);
  metrics_.submitted.fetch_add(1, std::memory_order_relaxed);
}

void NodeRuntime::EnterAsyncPending() noexcept {
  NodeState expected = NodeState::kReady;
  state_.compare_exchange_strong(expected, NodeState::kAsyncPending, std::memory_order_seq_cst);
}

bool NodeRuntime::TryLeaveAsyncPending() noexcept {
  NodeState expected = NodeState::kAsyncPending;
  return state_.compare_exchange_strong(expected, NodeState::kReady, std::memory_order_seq_cst);
}

bool NodeRuntime::EndAsync() noexcept {
  const std::uint32_t left = async_pending_.fetch_sub(1, std::memory_order_seq_cst) - 1;
  metrics_.completed.fetch_add(1, std::memory_order_relaxed);
  // Leave async_pending as soon as a slot under the cap is free; the
  // scheduler re-marks us (12 §4.3 step 6).
  return TryLeaveAsyncPending() || left == 0;
}

void NodeRuntime::EnterClosing() noexcept {
  state_.store(NodeState::kClosing, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// LatencyHistogram
// ---------------------------------------------------------------------------

std::uint64_t LatencyHistogram::BucketUpperNs(std::size_t bucket) noexcept {
  if (bucket + 1 >= kBuckets) return std::numeric_limits<std::uint64_t>::max();
  return std::uint64_t{1} << (bucket + 10);
}

void LatencyHistogram::Record(std::uint64_t ns) noexcept {
  std::size_t b = 0;
  while (b + 1 < kBuckets && ns >= BucketUpperNs(b)) ++b;
  buckets_[b].fetch_add(1, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
  sum_ns_.fetch_add(ns, std::memory_order_relaxed);
}

std::uint64_t LatencyHistogram::Quantile(double q) const noexcept {
  const std::uint64_t total = count_.load(std::memory_order_relaxed);
  if (total == 0) return 0;
  const auto target = static_cast<std::uint64_t>(static_cast<double>(total) * q);
  std::uint64_t seen = 0;
  for (std::size_t b = 0; b < kBuckets; ++b) {
    seen += buckets_[b].load(std::memory_order_relaxed);
    if (seen > target || seen == total) return BucketUpperNs(b);
  }
  return BucketUpperNs(kBuckets - 1);
}

// ---------------------------------------------------------------------------
// Operator default
// ---------------------------------------------------------------------------

Status Operator::Submit(const SubmitRequest&) {
  return Status::InvalidArgument("operator does not support asynchronous submit");
}

// ---------------------------------------------------------------------------
// BuiltinOperatorFactory
// ---------------------------------------------------------------------------

void BuiltinOperatorFactory::Register(CapabilityDescriptor descriptor, Maker maker) {
  for (Entry& e : entries_) {
    if (e.descriptor.op == descriptor.op) {
      e.descriptor = std::move(descriptor);
      e.maker = std::move(maker);
      return;
    }
  }
  entries_.push_back({std::move(descriptor), std::move(maker)});
}

const CapabilityDescriptor* BuiltinOperatorFactory::Describe(const OperatorKey& key) const {
  for (const Entry& e : entries_) {
    if (e.descriptor.op == key) return &e.descriptor;
  }
  return nullptr;
}

Result<std::unique_ptr<Operator>> BuiltinOperatorFactory::Create(const OperatorCreateArgs& args) {
  for (const Entry& e : entries_) {
    if (e.descriptor.op == args.key) {
      std::unique_ptr<Operator> op = e.maker(args);
      if (!op) return Status::Internal("factory returned null for " + args.key.ToString());
      return op;
    }
  }
  return Status::NotFound("operator '" + args.key.ToString() + "' is not registered");
}

const CapabilityDescriptor* CompositeOperatorFactory::Describe(const OperatorKey& key) const {
  for (OperatorFactory* f : factories_) {
    if (const CapabilityDescriptor* d = f->Describe(key)) return d;
  }
  return nullptr;
}

Result<std::unique_ptr<Operator>> CompositeOperatorFactory::Create(const OperatorCreateArgs& args) {
  for (OperatorFactory* f : factories_) {
    if (f->Describe(args.key) != nullptr) return f->Create(args);
  }
  return Status::NotFound("operator '" + args.key.ToString() + "' not found");
}

}  // namespace ge
