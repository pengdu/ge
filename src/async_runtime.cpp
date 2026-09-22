#include <ge/cpp/async_runtime.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <utility>

namespace ge {

namespace {

constexpr std::uint64_t kSinkMagic = 0x4745434f4d504c54ull;  // "GECOMPLT"

}  // namespace

// ---------------------------------------------------------------------------
// NodeBatchConfig
// ---------------------------------------------------------------------------

NodeBatchConfig NodeBatchConfig::FromOptions(const JsonValue& node_options) {
  NodeBatchConfig c;
  const JsonValue* b = node_options.is_object() ? node_options.Find("batch") : nullptr;
  if (b == nullptr) return c;
  if (b->is_bool()) {
    c.enabled = b->as_bool();
    return c;
  }
  if (!b->is_object()) return c;
  if (const auto e = b->GetBool("enabled")) c.enabled = *e;
  if (const auto m = b->GetInteger("max_batch"); m && *m >= 1) {
    c.max_batch = static_cast<std::uint32_t>(std::min<std::int64_t>(*m, 1024));
  }
  if (const auto t = b->GetInteger("timeout_ms"); t && *t >= 0) {
    c.timeout = std::chrono::milliseconds(*t);
  }
  return c;
}

// ---------------------------------------------------------------------------
// CompletionQueue
// ---------------------------------------------------------------------------

CompletionQueue::CompletionQueue(std::uint32_t capacity) : capacity_(std::max(1u, capacity)) {}

Status CompletionQueue::Push(CompletionEvent event) {
  {
    std::lock_guard lock(mutex_);
    if (events_.size() >= capacity_) {
      dropped_on_full_.fetch_add(1, std::memory_order_relaxed);
      return Status::ResourceExhausted("completion queue is full");
    }
    events_.push_back(std::move(event));
  }
  if (notify_) notify_();
  return Status::Ok();
}

void CompletionQueue::ForcePush(CompletionEvent event) {
  {
    std::lock_guard lock(mutex_);
    events_.push_back(std::move(event));
  }
  if (notify_) notify_();
}

std::vector<CompletionEvent> CompletionQueue::DrainAll() {
  std::lock_guard lock(mutex_);
  std::vector<CompletionEvent> out(std::make_move_iterator(events_.begin()), std::make_move_iterator(events_.end()));
  events_.clear();
  return out;
}

std::size_t CompletionQueue::size() const {
  std::lock_guard lock(mutex_);
  return events_.size();
}

// ---------------------------------------------------------------------------
// SessionCompletionSink
// ---------------------------------------------------------------------------

SessionCompletionSink::SessionCompletionSink(AsyncRuntime& runtime, SessionId session)
    : runtime_(runtime), session_(session), body_{kSinkMagic, this} {}

SessionCompletionSink::~SessionCompletionSink() {
  body_.magic = 0;
  body_.sink = nullptr;
}

SessionCompletionSink* SessionCompletionSink::FromHandle(ge_completion_sink_t* handle) noexcept {
  if (handle == nullptr || handle->magic != kSinkMagic || handle->sink == nullptr) return nullptr;
  return static_cast<SessionCompletionSink*>(handle->sink);
}

Status SessionCompletionSink::Push(CompletionEvent event) {
  if (!alive_.load(std::memory_order_acquire)) {
    return Status::Cancelled("completion sink of session " + std::to_string(session_) +
                             " is closed");
  }
  if (event.session_id == 0) event.session_id = session_;
  return runtime_.queue_.Push(std::move(event));
}

// ---------------------------------------------------------------------------
// AsyncRuntime: lifecycle
// ---------------------------------------------------------------------------

AsyncRuntime::AsyncRuntime(AsyncOptions options)
    : options_(options), queue_(options.completion_queue_capacity) {
  queue_.set_notify([this] { Wake(); });
  if (options_.worker_thread) worker_ = std::thread([this] { Worker(); });
}

AsyncRuntime::~AsyncRuntime() {
  {
    std::lock_guard lock(wake_mutex_);
    stop_ = true;
    wake_cv_.notify_all();
  }
  if (worker_.joinable()) worker_.join();
  queue_.set_notify(nullptr);
  std::lock_guard lock(state_mutex_);
  for (auto& [id, s] : sessions_) s.sink->Detach();
}

void AsyncRuntime::Wake() {
  std::lock_guard lock(wake_mutex_);
  wake_pending_ = true;
  wake_cv_.notify_all();
}

void AsyncRuntime::Worker() {
  for (;;) {
    {
      std::unique_lock lock(wake_mutex_);
      std::optional<std::chrono::steady_clock::time_point> next;
      {
        std::lock_guard state(state_mutex_);
        next = NextWake();
      }
      if (next) {
        wake_cv_.wait_until(lock, *next, [this] { return stop_ || wake_pending_; });
      } else {
        wake_cv_.wait(lock, [this] { return stop_ || wake_pending_; });
      }
      if (stop_) return;
      wake_pending_ = false;
    }
    (void)Pump();
  }
}

std::optional<std::chrono::steady_clock::time_point> AsyncRuntime::NextWake() const {
  std::optional<std::chrono::steady_clock::time_point> next;
  const auto consider = [&](std::chrono::steady_clock::time_point t) {
    if (!next || t < *next) next = t;
  };
  for (const auto& [key, members] : batches_) {
    for (const Pending& p : members) consider(p.due);
  }
  for (const auto& [key, buf] : reorder_) {
    if (buf.order.empty()) continue;
    const auto it = in_flight_.find(buf.order.front());
    if (it != in_flight_.end() && it->second.submitted && !it->second.done) {
      consider(it->second.deadline);
    }
  }
  if (!blocked_.empty()) consider(std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
  return next;
}

std::shared_ptr<SessionCompletionSink> AsyncRuntime::AttachSession(SessionId session,
                                                                   AsyncSessionHooks hooks,
                                                                   bool batching) {
  auto sink = std::make_shared<SessionCompletionSink>(*this, session);
  std::lock_guard lock(state_mutex_);
  sessions_[session] = SessionEntry{sink, std::move(hooks), batching, std::make_shared<HookGuard>()};
  return sink;
}

void AsyncRuntime::DetachSession(SessionId session) {
  std::shared_ptr<SessionCompletionSink> sink;
  SessionEntry entry;
  std::vector<Entry> dropped;
  {
    std::lock_guard lock(state_mutex_);
    const auto it = sessions_.find(session);
    if (it == sessions_.end()) return;
    entry = std::move(it->second);
    sink = entry.sink;
    sink->Detach();
    sessions_.erase(it);
    // Parked (never submitted) requests of the session are discarded.
    for (auto& [key, members] : batches_) {
      std::erase_if(members, [&](const Pending& p) {
        const auto e = in_flight_.find(p.id);
        return e != in_flight_.end() && e->second.session == session;
      });
    }
    std::erase_if(batches_, [](const auto& kv) { return kv.second.empty(); });
    for (auto it2 = in_flight_.begin(); it2 != in_flight_.end();) {
      if (it2->second.session != session) {
        ++it2;
        continue;
      }
      dropped.push_back(std::move(it2->second));
      it2 = in_flight_.erase(it2);
    }
    std::erase_if(reorder_, [&](const auto& kv) {
      return kv.second.order.empty() ||
             std::all_of(kv.second.order.begin(), kv.second.order.end(), [&](RequestId id) {
               return !in_flight_.contains(id);
             });
    });
    std::erase_if(blocked_, [&](const auto& kv) { return kv.second.session == session; });
  }
  // A late completion for these requests is an orphan (12 §4.3 step 1).
  for (Entry& e : dropped) {
    in_flight_count_.fetch_sub(1, std::memory_order_relaxed);
    orphan_total_.fetch_add(1, std::memory_order_relaxed);
    e.node->metrics().orphan_completions.fetch_add(1, std::memory_order_relaxed);
    if (e.node->EndAsync()) entry.OnAsyncIdle(*e.node);
  }
  // Last: block until every hook snapshot still running on the consumer
  // thread has returned, then make all of them no-ops. The caller
  // (~Session) frees the Session right after this.
  if (entry.guard) {
    std::unique_lock lock(entry.guard->mutex);
    entry.guard->alive = false;
  }
}

// ---------------------------------------------------------------------------
// Submit path (12 §4.2)
// ---------------------------------------------------------------------------

std::int32_t AsyncRuntime::DeviceOf(const RuntimeTopology& topology, const NodeRuntime& node) {
  const NodeSpec* spec = topology.spec().FindNode(node.external_id());
  if (spec != nullptr && spec->executor.device_id) return *spec->executor.device_id;
  if (topology.spec().options().default_device_id) return *topology.spec().options().default_device_id;
  return -1;
}

RequestId AsyncRuntime::Submit(SubmitArgs args) {
  const RequestId id = next_request_.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now();
  NodeRuntime& node = *args.node;
  const NodeBatchConfig cfg = NodeBatchConfig::FromOptions(node.options());
  std::uint64_t inference_ms = node.capability().execution.max_inference_ms.value_or(0);
  const auto inference_timeout =
      inference_ms == 0 ? options_.default_inference_timeout
                        : std::chrono::milliseconds(inference_ms);

  Entry entry;
  entry.id = id;
  entry.session = args.topology->session_id();
  entry.topology = args.topology->version();
  entry.parameter = args.parameters->version;
  entry.node = args.node;
  entry.topology_ref = args.topology;
  entry.seq = args.packet_seq;
  entry.ingress_ns = OldestIngress(args.inputs);
  entry.enqueued = now;
  entry.deadline = now + inference_timeout;  // refined at flush

  Pending pending;
  pending.id = id;
  pending.parameters = args.parameters;
  pending.input_ports = std::move(args.input_ports);
  pending.inputs = std::move(args.inputs);

  BatchKey key;
  key.operator_key = node.operator_key().ToString();
  key.device_id = args.device_id == -1 ? DeviceOf(*args.topology, node) : args.device_id;
  if (const auto m = node.options().GetString("model")) key.model = *m;
  key.parameters = args.parameters->values.Serialize();

  bool batch_now = true;
  std::vector<Pending> ready;
  {
    std::lock_guard lock(state_mutex_);
    const auto sit = sessions_.find(entry.session);
    const bool session_batching = sit != sessions_.end() && sit->second.batching;
    const std::uint32_t max_batch = cfg.max_batch.value_or(options_.max_batch);
    const auto timeout = cfg.timeout.value_or(options_.batch_timeout);
    const bool batching = options_.batching && session_batching && cfg.enabled && max_batch > 1;
    in_flight_.emplace(id, std::move(entry));
    reorder_[ReorderKey{&node, args.topology->version()}].order.push_back(id);
    in_flight_count_.fetch_add(1, std::memory_order_relaxed);
    if (batching) {
      pending.due = now + timeout;
      pending.max_batch = max_batch;
      std::vector<Pending>& members = batches_[key];
      members.push_back(std::move(pending));
      if (members.size() >= max_batch) {
        ready = std::move(members);
        batches_.erase(key);
      } else {
        batch_now = false;
      }
    } else {
      pending.max_batch = 1;
      ready.push_back(std::move(pending));
    }
  }
  if (batch_now) {
    FlushBatch(key, std::move(ready));
  } else {
    Wake();  // the worker's timer picks the batch up
  }
  return id;
}

void AsyncRuntime::FlushBatch(const BatchKey& key, std::vector<Pending> members) {
  (void)key;
  if (members.empty()) return;
  const std::uint64_t batch_id = next_batch_.fetch_add(1, std::memory_order_relaxed);
  const auto size = static_cast<std::uint32_t>(members.size());
  batches_flushed_.fetch_add(1, std::memory_order_relaxed);
  struct Failed {
    NodeRuntimeRef node;
    std::optional<SessionEntry> session;
    Status status;
  };
  std::vector<Failed> failed;
  std::lock_guard submit_lock(submit_mutex_);
  for (std::uint32_t i = 0; i < size; ++i) {
    Pending& p = members[i];
    NodeRuntimeRef node;
    std::shared_ptr<const RuntimeTopology> topo;
    SubmitRequest req;
    CompletionSink* sink = nullptr;
    std::optional<SessionEntry> session;
    {
      std::lock_guard lock(state_mutex_);
      const auto it = in_flight_.find(p.id);
      if (it == in_flight_.end()) continue;  // session detached meanwhile
      Entry& e = it->second;
      node = e.node;
      topo = e.topology_ref;
      const auto now = std::chrono::steady_clock::now();
      e.submitted = now;
      const std::uint64_t ms = node->capability().execution.max_inference_ms.value_or(0);
      e.deadline = now + (ms == 0 ? options_.default_inference_timeout
                                  : std::chrono::milliseconds(ms));
      req.request_id = e.id;
      req.session_id = e.session;
      req.topology_version = e.topology;
      req.parameter_version = e.parameter;
      req.packet_seq = e.seq;
      const auto sit = sessions_.find(e.session);
      if (sit != sessions_.end()) {
        sink = sit->second.sink.get();
        session = sit->second;
      }
    }
    node->metrics().batch_count.fetch_add(1, std::memory_order_relaxed);
    node->metrics().batch_size_sum.fetch_add(size, std::memory_order_relaxed);
    req.input_ports.reserve(p.input_ports.size());
    for (const std::string& port : p.input_ports) req.input_ports.emplace_back(port);
    req.inputs = std::move(p.inputs);
    req.parameters = &p.parameters->values;
    req.completion_sink = sink;
    req.batch_id = batch_id;
    req.batch_index = i;
    req.batch_size = size;
    Status st = node->cancelled() ? Status::Cancelled("node '" + node->external_id() + "' is retired")
                                  : node->op().Submit(req);
    if (!st.ok()) {
      // Synchronous failure: the node fails now (no further submits are
      // admitted) and the result still travels through the queue so the
      // request is retired with the same accounting as any completion.
      CompletionEvent ev;
      ev.request_id = req.request_id;
      ev.session_id = req.session_id;
      ev.topology_version = req.topology_version;
      ev.parameter_version = req.parameter_version;
      ev.packet_seq = req.packet_seq;
      ev.status = st;
      queue_.ForcePush(std::move(ev));
      if (!node->cancelled()) failed.push_back({node, std::move(session), std::move(st)});
    }
  }
  for (Failed& f : failed) {
    const NodeState s = f.node->state();
    if (s == NodeState::kFailed || s == NodeState::kClosed) continue;
    f.node->Fail(f.status);
    if (f.session) f.session->OnNodeFailed(*f.node, f.status);
  }
}

// ---------------------------------------------------------------------------
// Consumer (12 §4.3)
// ---------------------------------------------------------------------------

bool AsyncRuntime::Pump() {
  std::lock_guard pump(pump_mutex_);
  bool progressed = false;
  const auto now = std::chrono::steady_clock::now();
  progressed = FlushDueBatches(now) || progressed;
  progressed = RetryBlockedOutputs() || progressed;
  progressed = DrainCompletions() || progressed;
  progressed = ExpireDeadlines(std::chrono::steady_clock::now()) || progressed;
  progressed = DeliverReady() || progressed;
  return progressed;
}

std::size_t AsyncRuntime::blocked_outputs() const {
  std::lock_guard lock(state_mutex_);
  std::size_t n = 0;
  for (const auto& [key, b] : blocked_) n += b.pushes.size();
  return n;
}

// Re-pushes parked packets to the edges that reported WOULD_BLOCK. A node
// whose output drained completely becomes deliverable again.
bool AsyncRuntime::RetryBlockedOutputs() {
  std::vector<std::pair<ReorderKey, BlockedOutput>> work;
  {
    std::lock_guard lock(state_mutex_);
    for (auto& [key, b] : blocked_) work.emplace_back(key, b);
  }
  bool any = false;
  for (auto& [key, b] : work) {
    EmitReport report;
    std::vector<std::pair<EdgeChannel*, PacketRef>> still;
    const bool cancelled = b.node->cancelled();
    for (auto& [edge, packet] : b.pushes) {
      if (cancelled) continue;  // fast retire: drop
      // Same-edge FIFO: once a packet for this edge stayed blocked in this
      // round, later ones queue behind it even if a slot frees meanwhile.
      if (std::any_of(still.begin(), still.end(), [&](const auto& p) { return p.first == edge; })) {
        still.emplace_back(edge, packet);
        continue;
      }
      const PushOutcome o = edge->Push(packet);
      if (o == PushOutcome::kWouldBlock) {
        still.emplace_back(edge, packet);
        continue;
      }
      any = true;
      if (o == PushOutcome::kAccepted || o == PushOutcome::kAcceptedDroppedOldest) {
        b.node->metrics().packets_out.fetch_add(1, std::memory_order_relaxed);
        if (const std::shared_ptr<NodeRuntime> t = edge->consumer()) report.targets.push_back(t.get());
      }
    }
    std::optional<SessionEntry> session;
    {
      std::lock_guard lock(state_mutex_);
      const auto it = blocked_.find(key);
      if (it != blocked_.end()) {
        if (still.empty()) {
          blocked_.erase(it);
        } else {
          it->second.pushes = std::move(still);
        }
      }
      if (const auto sit = sessions_.find(b.session); sit != sessions_.end()) session = sit->second;
    }
    if (!report.targets.empty() && session) session->AfterEmit(report);
  }
  return any;
}

bool AsyncRuntime::FlushDueBatches(std::chrono::steady_clock::time_point now) {
  std::vector<std::pair<BatchKey, std::vector<Pending>>> due;
  {
    std::lock_guard lock(state_mutex_);
    for (auto it = batches_.begin(); it != batches_.end();) {
      const bool is_due = std::any_of(it->second.begin(), it->second.end(),
                                      [&](const Pending& p) { return p.due <= now; });
      if (!is_due) {
        ++it;
        continue;
      }
      due.emplace_back(it->first, std::move(it->second));
      it = batches_.erase(it);
    }
  }
  for (auto& [key, members] : due) FlushBatch(key, std::move(members));
  return !due.empty();
}

bool AsyncRuntime::DrainCompletions() {
  std::vector<CompletionEvent> events = queue_.DrainAll();
  for (CompletionEvent& ev : events) {
    // 12 §4.3: every event runs steps 1-7 before the next one is looked
    // at, so a result that became deliverable is routed before a later
    // error can fail its node.
    OnCompletion(std::move(ev));
    (void)DeliverReady();
  }
  return !events.empty();
}

void AsyncRuntime::OnCompletion(CompletionEvent event) {
  NodeRuntimeRef failed_node;
  std::optional<SessionEntry> failed_session;
  Status failure;
  {
    std::lock_guard lock(state_mutex_);
    const auto it = in_flight_.find(event.request_id);
    const auto orphan = [&](NodeRuntime* node) {
      orphan_total_.fetch_add(1, std::memory_order_relaxed);
      if (node != nullptr) node->metrics().orphan_completions.fetch_add(1, std::memory_order_relaxed);
    };
    if (it == in_flight_.end()) {
      orphan(nullptr);  // unknown / already finished request (step 1)
      return;
    }
    Entry& e = it->second;
    if (e.done) {
      // Duplicate completion for a request already timed out or delivered.
      orphan(e.node.get());
      late_total_.fetch_add(1, std::memory_order_relaxed);
      e.node->metrics().late_completions.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (event.session_id != e.session || event.topology_version != e.topology ||
        event.parameter_version != e.parameter || event.packet_seq != e.seq) {
      orphan(e.node.get());  // step 2: version mismatch
      return;
    }
    if (e.submitted) {
      e.node->metrics().async_wait.Record(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - *e.submitted).count()));
    }
    if (!event.status.ok() && !e.node->cancelled()) {
      // An error aborts the node as soon as it is observed: results ahead
      // of it in the reorder buffer are not worth waiting for and results
      // behind it are never delivered (they finish as orphans).
      failed_node = e.node;
      failure = event.status;
      if (const auto sit = sessions_.find(e.session); sit != sessions_.end()) {
        failed_session = sit->second;
      }
    }
    e.result = std::move(event);
    e.done = true;
  }
  if (failed_node) {
    const NodeState s = failed_node->state();
    if (s != NodeState::kFailed && s != NodeState::kClosed) {
      failed_node->Fail(failure);
      if (failed_session) failed_session->OnNodeFailed(*failed_node, failure);
    }
  }
}

bool AsyncRuntime::ExpireDeadlines(std::chrono::steady_clock::time_point now) {
  bool any = false;
  std::lock_guard lock(state_mutex_);
  for (auto& [key, buf] : reorder_) {
    if (buf.order.empty()) continue;
    const auto it = in_flight_.find(buf.order.front());
    if (it == in_flight_.end()) continue;
    Entry& e = it->second;
    if (e.done || !e.submitted || e.deadline > now) continue;
    // 12 §6.4: head deadline expired -> logical gap, advance.
    e.done = true;
    e.result.reset();
    any = true;
  }
  return any;
}

bool AsyncRuntime::DeliverReady() {
  bool any = false;
  for (;;) {
    std::optional<Entry> ready;
    SessionEntry session;
    bool have_session = false;
    {
      std::lock_guard lock(state_mutex_);
      for (auto it = reorder_.begin(); it != reorder_.end();) {
        ReorderBuffer& buf = it->second;
        while (!buf.order.empty() && !in_flight_.contains(buf.order.front())) buf.order.pop_front();
        if (buf.order.empty()) {
          it = reorder_.erase(it);
          continue;
        }
        if (blocked_.contains(it->first)) {
          ++it;  // earlier output still waiting for a slot: keep order
          continue;
        }
        const auto eit = in_flight_.find(buf.order.front());
        if (eit->second.done) {
          ready = std::move(eit->second);
          in_flight_.erase(eit);
          buf.order.pop_front();
          const auto sit = sessions_.find(ready->session);
          if (sit != sessions_.end()) {
            session = sit->second;
            have_session = true;
          }
          break;
        }
        ++it;
      }
    }
    if (!ready) return any;
    any = true;
    in_flight_count_.fetch_sub(1, std::memory_order_relaxed);
    if (ready->result) {
      DeliverEntry(*ready, have_session ? &session : nullptr);
    } else {
      EmitGap(*ready, have_session ? &session : nullptr);
    }
    FinishEntry(*ready, have_session ? &session : nullptr);
  }
}

void AsyncRuntime::DeliverEntry(Entry& entry, const SessionEntry* session) {
  CompletionEvent& ev = *entry.result;
  NodeRuntime& node = *entry.node;
  if (!ev.status.ok()) {
    if (!node.cancelled()) FailNode(entry, session, ev.status);  // already failed in OnCompletion
    return;
  }
  // Step 3: a fast-retired topology or cancelled node never receives.
  if (node.cancelled() || node.state() == NodeState::kClosed || node.state() == NodeState::kFailed) {
    orphan_total_.fetch_add(1, std::memory_order_relaxed);
    node.metrics().orphan_completions.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  EmitReport report;
  BlockedOutput parked;
  for (CompletionOutput& out : ev.outputs) {
    if (out.packet.is_eos() || out.packet.is_dropped()) continue;  // engine-only markers
    node.NoteSeq(out.packet.header.seq);
    EmitReport one;
    if (!parked.pushes.empty()) {
      // An earlier output of this result is waiting for the same port's
      // edges: queue behind it (same-edge FIFO) instead of overtaking.
      const auto* routes = entry.topology_ref->RoutesFor(node.id(), out.output_port);
      const bool same_edge = routes != nullptr &&
                             std::any_of(routes->begin(), routes->end(), [&](const RouteEntry& r) {
                               return std::any_of(parked.pushes.begin(), parked.pushes.end(),
                                                  [&](const auto& p) { return p.first == r.edge.get(); });
                             });
      if (same_edge) {
        out.packet.header.topology_version = entry.topology;
        out.packet.header.parameter_version = entry.parameter;
        if (out.packet.ingress_ns == 0) out.packet.ingress_ns = entry.ingress_ns;
        if (out.packet.header.type_tag == kInvalidTypeTag) out.packet.header.type_tag = routes->front().type_tag;
        PacketRef shared = std::make_shared<const Packet>(std::move(out.packet));
        for (const RouteEntry& r : *routes) parked.pushes.emplace_back(r.edge.get(), shared);
        continue;
      }
    }
    if (out.packet.ingress_ns == 0) out.packet.ingress_ns = entry.ingress_ns;
    Status st = PacketRouter::Emit(*entry.topology_ref, node, out.output_port, std::move(out.packet),
                                   entry.parameter, &one);
    report.accepted += one.accepted;
    report.dropped += one.dropped;
    report.would_block += one.would_block;
    report.cancelled += one.cancelled;
    report.targets.insert(report.targets.end(), one.targets.begin(), one.targets.end());
    if (st.code() == GE_STATUS_WOULD_BLOCK) {
      // 12 §6.2 block policy: keep the packet for the full edges and retry
      // from the consumer loop instead of dropping it.
      for (EdgeChannel* edge : one.blocked) parked.pushes.emplace_back(edge, one.packet);
      continue;
    }
    if (!st.ok() && st.code() != GE_STATUS_CANCELLED) {
      FailNode(entry, session, st);
      return;
    }
  }
  if (!parked.pushes.empty()) {
    parked.node = entry.node;
    parked.topology = entry.topology_ref;
    parked.session = entry.session;
    std::lock_guard lock(state_mutex_);
    BlockedOutput& slot = blocked_[ReorderKey{&node, entry.topology}];
    if (slot.pushes.empty()) {
      slot = std::move(parked);
    } else {
      slot.pushes.insert(slot.pushes.end(), parked.pushes.begin(), parked.pushes.end());
    }
  }
  if (session != nullptr) session->AfterEmit(report);
}

void AsyncRuntime::EmitGap(Entry& entry, const SessionEntry* session) {
  NodeRuntime& node = *entry.node;
  gap_total_.fetch_add(1, std::memory_order_relaxed);
  node.metrics().reorder_gaps.fetch_add(1, std::memory_order_relaxed);
  node.metrics().errors.fetch_add(1, std::memory_order_relaxed);
  if (node.cancelled()) return;
  // ASY-4: a placeholder with dropped/timeout metadata on every output.
  EmitReport report;
  for (const std::string& port : entry.topology_ref->OutputPorts(node.id())) {
    Packet gap;
    gap.header.seq = entry.seq;
    gap.header.flags = GE_PACKET_FLAG_DROPPED;
    auto meta = std::make_shared<Metadata>();
    meta->Set(metadata_keys::kDropped, "true");
    meta->Set(metadata_keys::kTimeout, "true");
    meta->Set("request_id", std::to_string(entry.id));
    gap.metadata = std::move(meta);
    (void)PacketRouter::Emit(*entry.topology_ref, node, port, std::move(gap), entry.parameter,
                             &report);
  }
  if (session != nullptr) session->AfterEmit(report);
}

void AsyncRuntime::FailNode(Entry& entry, const SessionEntry* session, const Status& status) {
  NodeRuntime& node = *entry.node;
  const NodeState s = node.state();
  if (s == NodeState::kFailed || s == NodeState::kClosed) return;
  node.Fail(status);
  if (session != nullptr) session->OnNodeFailed(node, status);
}

void AsyncRuntime::FinishEntry(Entry& entry, const SessionEntry* session) {
  NodeRuntime& node = *entry.node;
  const bool wake = node.EndAsync();
  if (wake && session != nullptr) session->OnAsyncIdle(node);
}

JsonValue AsyncRuntime::Stats() const {
  JsonObject o;
  o["in_flight"] = JsonValue(in_flight());
  o["orphan_completion_total"] = JsonValue(orphan_completion_total());
  o["timeout_gap_total"] = JsonValue(timeout_gap_total());
  o["late_completion_total"] = JsonValue(late_completion_total());
  o["batches_flushed"] = JsonValue(batches_flushed());
  o["queue_dropped_on_full"] = JsonValue(queue_.dropped_on_full());
  o["queue_depth"] = JsonValue(static_cast<std::uint64_t>(queue_.size()));
  std::size_t parked = 0;
  {
    std::lock_guard lock(state_mutex_);
    for (const auto& [k, m] : batches_) parked += m.size();
  }
  o["batch_parked"] = JsonValue(static_cast<std::uint64_t>(parked));
  o["blocked_outputs"] = JsonValue(static_cast<std::uint64_t>(blocked_outputs()));
  return JsonValue(std::move(o));
}

}  // namespace ge
