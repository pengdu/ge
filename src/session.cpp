#include <ge/cpp/session.h>

#include <algorithm>
#include <chrono>
#include <set>

#include <ge/cpp/graph_spec_json.h>

namespace ge {

std::string_view ToString(SessionState s) noexcept {
  switch (s) {
    case SessionState::kCreated: return "created";
    case SessionState::kStarting: return "starting";
    case SessionState::kRunning: return "running";
    case SessionState::kPausing: return "pausing";
    case SessionState::kPaused: return "paused";
    case SessionState::kStopping: return "stopping";
    case SessionState::kStopped: return "stopped";
    case SessionState::kFailed: return "failed";
  }
  return "unknown";
}

namespace {

JsonValue Strings(const std::vector<std::string>& v) {
  JsonArray a;
  for (const std::string& s : v) a.emplace_back(s);
  return JsonValue(std::move(a));
}

}  // namespace

JsonValue DryRunResult::ToJson() const {
  JsonObject o;
  o["candidate"] = GraphSpecParser::ToJson(candidate);
  JsonObject contracts;
  for (const auto& [id, c] : validated.edge_contracts) contracts[id] = c.ToJson();
  o["edge_contracts"] = JsonValue(std::move(contracts));
  o["diff"] = diff.ToJson();
  // Flat lists kept for callers that only need the headline.
  o["added_nodes"] = Strings(diff.Nodes(GraphDiff::NodeChange::kAdded));
  o["removed_nodes"] = Strings(diff.Nodes(GraphDiff::NodeChange::kRemoved));
  o["added_edges"] = Strings(diff.Edges(GraphDiff::EdgeChange::kAdded));
  o["removed_edges"] = Strings(diff.Edges(GraphDiff::EdgeChange::kRemoved));
  return JsonValue(std::move(o));
}

// ---------------------------------------------------------------------------
// MutationCoordinator
// ---------------------------------------------------------------------------

MutationCoordinator::MutationCoordinator(Session& session, OperatorFactory& factory,
                                         OperationRegistry& operations)
    : session_(session),
      factory_(factory),
      operations_(operations),
      applier_([&factory](const OperatorKey& k) { return factory.Describe(k); }) {}

MutationCoordinator::~MutationCoordinator() { StopThread(); }

void MutationCoordinator::Submit(MutationRequest request) {
  {
    std::lock_guard lock(queue_mutex_);
    queue_.push_back(std::move(request));
  }
  queue_cv_.notify_one();
}

void MutationCoordinator::StartThread() {
  std::lock_guard lock(queue_mutex_);
  if (thread_.joinable()) return;
  stop_thread_ = false;
  thread_ = std::thread([this] { Loop(); });
}

void MutationCoordinator::StopThread() {
  {
    std::lock_guard lock(queue_mutex_);
    stop_thread_ = true;
  }
  queue_cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void MutationCoordinator::Loop() {
  for (;;) {
    {
      std::unique_lock lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return stop_thread_ || !queue_.empty(); });
      if (stop_thread_) return;
    }
    (void)Pump();
  }
}

void MutationCoordinator::CancelAll(std::string reason) {
  std::deque<MutationRequest> drop;
  {
    std::lock_guard lock(queue_mutex_);
    drop.swap(queue_);
  }
  for (const MutationRequest& r : drop) operations_.Cancel(r.operation, reason);
}

namespace {

// A1 / B1: a patch pinned to a base version only applies against that version.
Status BaseVersionCheck(const MutationPatch& patch, TopologyVersion current) {
  if (patch.base_topology_version && *patch.base_topology_version != current) {
    return Status::VersionConflict("patch base_topology_version " + std::to_string(*patch.base_topology_version) +
                                   " != current " + std::to_string(current));
  }
  return Status::Ok();
}

}  // namespace

Result<CandidateSpec> MutationCoordinator::ApplyPatch(const RuntimeTopology& base,
                                                      const MutationPatch& patch) const {
  auto r = applier_.Apply(base.spec(), patch);
  if (!r.ok()) return r.status();
  MigrateParameters(base, patch, &r->candidate);
  return r;
}

// 12 §7.5 step 2: ReplaceNode inherits parameters the *new* operator marks
// migratable, unless the request sets them explicitly. Live (hot-updated)
// values come from the ParameterStore, not the original spec.
void MutationCoordinator::MigrateParameters(const RuntimeTopology& base,
                                            const MutationPatch& patch,
                                            GraphSpec* candidate) const {
  for (const MutationAction& a : patch.actions) {
    const auto* rep = std::get_if<ReplaceNodeAction>(&a);
    if (rep == nullptr) continue;
    NodeRuntime* old = base.FindNode(rep->node);
    NodeSpec* fresh = candidate->FindNode(rep->node);
    const CapabilityDescriptor* cap = factory_.Describe(rep->replacement_op);
    if (old == nullptr || fresh == nullptr || cap == nullptr) continue;
    const auto live = old->parameters().Current();
    JsonObject merged = fresh->options.is_object() ? fresh->options.as_object() : JsonObject{};
    for (const std::string& key : cap->parameters.migratable) {
      if (rep->replacement_options.is_object() &&
          rep->replacement_options.as_object().contains(key)) {
        continue;
      }
      if (!live->values.is_object()) continue;
      const auto it = live->values.as_object().find(key);
      if (it != live->values.as_object().end()) merged[key] = it->second;
    }
    fresh->options = JsonValue(std::move(merged));
  }
}

Result<DryRunResult> MutationCoordinator::DryRun(const RuntimeTopology& base,
                                                 const MutationPatch& patch) const {
  if (Status s = BaseVersionCheck(patch, base.version()); !s.ok()) return s;
  auto changes = ApplyPatch(base, patch);
  if (!changes.ok()) return changes.status();
  GraphValidator validator([this](const OperatorKey& k) { return factory_.Describe(k); });
  auto validated = validator.Validate(changes->candidate);
  if (!validated.ok()) return validated.status();
  DryRunResult r;
  r.diff = GraphDiff::Compute(base.spec(), base.validated().edge_contracts, changes->candidate,
                              validated->edge_contracts);
  r.candidate = std::move(changes->candidate);
  r.validated = std::move(*validated);
  return r;
}

bool MutationCoordinator::Disjoint(const CandidateSpec& a, const CandidateSpec& b) {
  const std::set<std::string> an(a.touched_nodes.begin(), a.touched_nodes.end());
  const std::set<std::string> ae(a.touched_edges.begin(), a.touched_edges.end());
  for (const std::string& n : b.touched_nodes) {
    if (an.contains(n)) return false;
  }
  for (const std::string& e : b.touched_edges) {
    if (ae.contains(e)) return false;
  }
  return true;
}

// 12 §7.1 merge: queue head plus following requests while base versions,
// remove policy match and touched sets are pairwise disjoint.
MutationCoordinator::Merged MutationCoordinator::TakeBatch() {
  Merged m;
  std::lock_guard lock(queue_mutex_);
  if (queue_.empty()) return m;
  const std::shared_ptr<RuntimeTopology> base = session_.current_topology();
  const std::size_t limit = std::max<std::size_t>(1, session_.options().mutation_merge_limit);
  std::vector<CandidateSpec> footprints;
  while (!queue_.empty() && m.origins.size() < limit) {
    MutationRequest& head = queue_.front();
    if (!m.origins.empty()) {
      const MutationPatch& first = m.origins.front().patch;
      if (head.patch.remove_policy != first.remove_policy) break;
      if (head.patch.base_topology_version != first.base_topology_version) break;
      auto fp = applier_.Apply(base->spec(), head.patch);
      if (!fp.ok()) break;  // let it fail on its own turn with its own error
      bool disjoint = true;
      for (const CandidateSpec& f : footprints) disjoint = disjoint && Disjoint(f, *fp);
      if (!disjoint) break;
      footprints.push_back(std::move(*fp));
    } else {
      auto fp = applier_.Apply(base->spec(), head.patch);
      if (fp.ok()) footprints.push_back(std::move(*fp));
      m.patch.remove_policy = head.patch.remove_policy;
      m.patch.base_topology_version = head.patch.base_topology_version;
    }
    for (const MutationAction& a : head.patch.actions) m.patch.actions.push_back(a);
    m.origins.push_back(std::move(head));
    queue_.pop_front();
    if (footprints.size() != m.origins.size()) break;  // head failed to apply: alone
  }
  return m;
}

bool MutationCoordinator::Pump() {
  std::lock_guard exec(execute_mutex_);
  Merged batch = TakeBatch();
  if (batch.origins.empty()) return false;
  Execute(std::move(batch));
  return true;
}

void MutationCoordinator::FailAll(const Merged& batch, const Status& status,
                                  const std::string& action_detail) {
  for (const MutationRequest& r : batch.origins) {
    JsonObject d;
    if (!action_detail.empty()) d["failed_action"] = JsonValue(action_detail);
    if (batch.origins.size() > 1) {
      JsonArray ids;
      for (const MutationRequest& o : batch.origins) ids.emplace_back(o.operation);
      d["merged_operations"] = JsonValue(std::move(ids));
    }
    if (!status.context_json().empty()) {
      auto ctx = ParseJson(status.context_json());
      d["context"] = ctx.ok() ? std::move(*ctx.value) : JsonValue(status.context_json());
    }
    operations_.Fail(r.operation, status, JsonValue(std::move(d)));
  }
}

Result<MutationCoordinator::Prepared> MutationCoordinator::Prepare(const RuntimeTopology& base,
                                                                   const MutationPatch& patch,
                                                                   TopologyVersion version) {
  // A2–A3
  auto changes = ApplyPatch(base, patch);
  if (!changes.ok()) return changes.status();
  // A5': validate + negotiate once, diff against the running version, and
  // let the diff alone decide what is carried over (kept/updated nodes, kept
  // edges) versus re-created. A4–A5 (+ A8; A6 resources arrive with P8) --
  // Build creates operator instances without opening them.
  GraphValidator validator([this](const OperatorKey& k) { return factory_.Describe(k); });
  auto validated = validator.Validate(changes->candidate);
  if (!validated.ok()) return validated.status();
  GraphDiff diff = GraphDiff::Compute(base.spec(), base.validated().edge_contracts,
                                      changes->candidate, validated->edge_contracts);
  RuntimeTopology::BuildOptions bo;
  bo.session_id = session_.id();
  bo.version = version;
  bo.aligned = session_.options().aligned;
  bo.base = &base;
  bo.reused_nodes = diff.ReusedNodes();
  bo.reused_edges = diff.ReusedEdges();
  bo.validated = &*validated;
  auto built = RuntimeTopology::Build(changes->candidate, factory_, bo);
  if (!built.ok()) return built.status();
  // A7 warm-up.
  if (Status s = session_.scheduler().OpenNodes(**built); !s.ok()) {
    session_.scheduler().AbandonCandidate(**built);
    return s;
  }
  Prepared p;
  p.candidate = std::move(*built);
  p.changes = std::move(*changes);
  p.diff = std::move(diff);
  p.policy = patch.remove_policy;
  return p;
}

void MutationCoordinator::Execute(Merged batch) {
  for (const MutationRequest& r : batch.origins) operations_.SetRunning(r.operation);
  if (!session_.AcceptsMutations()) {
    FailAll(batch, session_.NotMutable(), "");
    return;
  }
  const std::shared_ptr<RuntimeTopology> base = session_.current_topology();
  // A1 + B1 base_version check. Publish is serialised by execute_mutex_, so
  // a version seen here is the version we publish against.
  if (Status s = BaseVersionCheck(batch.patch, base->version()); !s.ok()) {
    FailAll(batch, std::move(s), "");
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  auto prepared = Prepare(*base, batch.patch, base->version() + 1);
  const auto t_prepared = std::chrono::steady_clock::now();
  if (!prepared.ok()) {
    // Attribute the failure to an action when the message names one.
    std::string action;
    for (const MutationAction& a : batch.patch.actions) {
      const std::string name(ActionTypeName(a));
      if (prepared.status().message().find(name) != std::string::npos) {
        action = name;
        break;
      }
    }
    FailAll(batch, prepared.status(), action);
    return;
  }

  // Parameter-only actions (set_node_options) ride along: validated per node
  // now, applied at the packet boundary (12 §5). Any rejection fails the
  // whole batch before publish (MUT-2).
  std::vector<std::pair<NodeRuntime*, ParameterUpdateRequest>> param_updates;
  for (const SetNodeOptionsAction& u : prepared->changes.parameter_updates) {
    NodeRuntime* node = prepared->candidate->FindNode(u.node);
    if (node == nullptr) {
      session_.scheduler().AbandonCandidate(*prepared->candidate);
      FailAll(batch, Status::NotFound("set_node_options: node '" + u.node + "' not found"),
              "set_node_options");
      return;
    }
    if (!u.parameters.is_object()) {
      session_.scheduler().AbandonCandidate(*prepared->candidate);
      FailAll(batch, Status::InvalidArgument("set_node_options: parameters must be an object"),
              "set_node_options");
      return;
    }
    const auto& hot = node->capability().parameters.hot_updatable;
    for (const auto& [k, _] : u.parameters.as_object()) {
      if (std::find(hot.begin(), hot.end(), k) == hot.end()) {
        session_.scheduler().AbandonCandidate(*prepared->candidate);
        FailAll(batch,
                Status::ParameterUnsupported("parameter '" + k + "' of node '" + u.node +
                                             "' is not hot-updatable"),
                "set_node_options");
        return;
      }
    }
    param_updates.emplace_back(node, ParameterUpdateRequest{batch.origins.front().operation, u.parameters});
  }

  // B1–B3
  RetireRequest retire;
  retire.policy = prepared->policy;
  retire.drain_timeout = session_.options().drain_timeout;
  const TopologyVersion published = prepared->candidate->version();
  std::vector<OperationId> ops;
  for (const MutationRequest& r : batch.origins) ops.push_back(r.operation);
  OperationRegistry* registry = &operations_;
  SessionEvents* events = &session_.events();
  // Success detail (AUD-1 / OBS-3): the structural diff that was applied and
  // the phase costs. |prepare_ms| covers A2–A7 (validate, negotiate, warm-up);
  // |retire_ms| runs from the swap to the last removed node closing (§4.1
  // "拓扑生效" is the swap itself, which is inside prepare→retire).
  JsonObject detail;
  detail["diff"] = prepared->diff.ToJson();
  detail["prepare_ms"] = JsonValue(std::chrono::duration<double, std::milli>(t_prepared - t0).count());
  const auto t_publish = std::chrono::steady_clock::now();
  retire.on_complete = [registry, ops, published, detail = std::move(detail), t_publish](bool timed_out) {
    JsonObject d = detail;
    d["retire_ms"] = JsonValue(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_publish).count());
    if (timed_out) d["drain_timeout_upgraded_to_fast"] = JsonValue(true);
    for (const OperationId id : ops) registry->Succeed(id, published, std::nullopt, JsonValue(d));
  };
  for (auto& [node, req] : param_updates) {
    (void)node->parameters().Submit(std::move(req), node->capability().parameters.hot_updatable);
  }
  if (Status s = session_.scheduler().Publish(prepared->candidate, std::move(retire)); !s.ok()) {
    // Stop() won the race after warm-up: same terminal state as the queued
    // requests CancelAll() dropped.
    session_.scheduler().AbandonCandidate(*prepared->candidate);
    for (const MutationRequest& r : batch.origins) operations_.Cancel(r.operation, std::string(s.message()));
    return;
  }
  for (const OperationId id : ops) operations_.SetTopologyVersion(id, published);
  if (events->on_topology_published) events->on_topology_published(published);
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

Result<std::unique_ptr<Session>> Session::Create(const GraphSpec& spec, OperatorFactory& factory,
                                                 ExecutorPool& executor,
                                                 OperationRegistry& operations,
                                                 SessionOptions options, SessionEvents events) {
  RuntimeTopology::BuildOptions bo;
  bo.session_id = options.id;
  bo.version = 1;
  bo.aligned = options.aligned;
  auto topo = RuntimeTopology::Build(spec, factory, bo);
  if (!topo.ok()) return topo.status();
  std::unique_ptr<Session> s(new Session(std::move(*topo), factory, executor, operations,
                                         std::move(options), std::move(events)));
  return s;
}

Session::Session(std::shared_ptr<RuntimeTopology> topology, OperatorFactory& factory,
                 ExecutorPool& executor, OperationRegistry& operations, SessionOptions options,
                 SessionEvents events)
    : options_(std::move(options)),
      events_(std::move(events)),
      operations_(operations),
      scheduler_(std::move(topology), executor,
                 SchedulerEvents{
                     [this](NodeRuntime& n, const Status& st) { OnNodeFailed(n, st); },
                     [this] { OnAllClosed(); },
                     [this](TopologyVersion v) {
                       if (events_.on_drain_timeout) events_.on_drain_timeout(v);
                     },
                     [this](NodeRuntime& n, std::string type, Severity sev, JsonValue detail) {
                       if (events_.on_operator_event) {
                         events_.on_operator_event(n, std::move(type), sev, std::move(detail));
                       }
                     }}),
      coordinator_(*this, factory, operations) {
  if (AsyncRuntime* rt = options_.async_runtime; rt != nullptr) {
    AsyncSessionHooks hooks;
    hooks.after_emit = [this](const EmitReport& r) { scheduler_.OnAsyncEmit(r); };
    hooks.on_async_idle = [this](NodeRuntime& n) { scheduler_.OnAsyncIdle(n); };
    hooks.on_node_failed = [this](NodeRuntime& n, const Status& st) {
      scheduler_.OnAsyncNodeFailed(n, st);
    };
    sink_ = rt->AttachSession(options_.id, std::move(hooks), options_.batching);
    AsyncDispatch dispatch;
    dispatch.submit = [rt](AsyncDispatch::Request req) {
      AsyncRuntime::SubmitArgs args;
      args.node = std::move(req.node);
      args.topology = std::move(req.topology);
      args.parameters = std::move(req.parameters);
      args.input_ports = std::move(req.input_ports);
      args.inputs = std::move(req.inputs);
      args.packet_seq = req.packet_seq;
      (void)rt->Submit(std::move(args));
    };
    scheduler_.SetAsyncDispatch(std::move(dispatch));
  }
}

Session::~Session() {
  coordinator_.StopThread();
  coordinator_.CancelAll("session destroyed");
  const SessionState s = state();
  if (s != SessionState::kStopped && s != SessionState::kCreated) {
    scheduler_.Stop(true);
    (void)scheduler_.WaitClosed(5000);
  }
  // 13 §7.3: the sink outlives every node; late completions become orphans.
  if (options_.async_runtime != nullptr) options_.async_runtime->DetachSession(options_.id);
}

void Session::Transition(SessionState to) {
  const SessionState from = state_.exchange(to, std::memory_order_acq_rel);
  if (from != to && events_.on_state_changed) events_.on_state_changed(from, to);
}

bool Session::TransitionIf(SessionState from, SessionState to) {
  SessionState expected = from;
  if (!state_.compare_exchange_strong(expected, to, std::memory_order_acq_rel)) return false;
  if (events_.on_state_changed) events_.on_state_changed(from, to);
  return true;
}

Status Session::NotMutable() const {
  return Status::InvalidArgument("session " + std::to_string(id()) + " is " +
                                 std::string(ToString(state())) +
                                 "; mutations and parameter updates need running/paused");
}

Status Session::Start() {
  if (!TransitionIf(SessionState::kCreated, SessionState::kStarting)) {
    return Status::InvalidArgument("session cannot start from state " +
                                   std::string(ToString(state())));
  }
  if (Status s = scheduler_.OpenAll(); !s.ok()) {
    failure_ = s;
    Transition(SessionState::kFailed);
    return s;
  }
  Transition(SessionState::kRunning);
  scheduler_.Start();
  if (options_.coordinator_thread) coordinator_.StartThread();
  return Status::Ok();
}

Status Session::Pause() {
  if (!TransitionIf(SessionState::kRunning, SessionState::kPausing)) {
    return Status::InvalidArgument("session cannot pause from state " +
                                   std::string(ToString(state())));
  }
  scheduler_.Pause();
  Transition(SessionState::kPaused);
  return Status::Ok();
}

Status Session::Resume() {
  if (!TransitionIf(SessionState::kPaused, SessionState::kRunning)) {
    return Status::InvalidArgument("session cannot resume from state " +
                                   std::string(ToString(state())));
  }
  scheduler_.Resume();
  return Status::Ok();
}

Result<OperationId> Session::Stop(bool fast, CallerContext caller) {
  const OperationId op = operations_.Create("session.stop", id(), std::move(caller),
                                            JsonValue(JsonObject{{"fast", JsonValue(fast)}}));
  SessionState from = state();
  for (;;) {
    if (from == SessionState::kStopping || from == SessionState::kStopped ||
        from == SessionState::kFailed) {
      // Already stopping/stopped (explicit stop, natural EOS end or failure).
      std::lock_guard lock(stop_mutex_);
      if (stopped_signalled_) {
        operations_.Succeed(op);
      } else {
        stop_waiters_.push_back(op);
      }
      return op;
    }
    if (from == SessionState::kCreated) {
      // Never started: nothing to drain.
      if (TransitionIf(from, SessionState::kStopped)) {
        std::lock_guard lock(stop_mutex_);
        stopped_signalled_ = true;
        operations_.Succeed(op);
        stop_cv_.notify_all();
        return op;
      }
      from = state();
      continue;
    }
    if (TransitionIf(from, SessionState::kStopping)) break;
    from = state();
  }
  {
    std::lock_guard lock(stop_mutex_);
    stop_waiters_.push_back(op);
  }
  operations_.SetRunning(op);
  coordinator_.CancelAll("session stopping");
  if (scheduler_.all_closed()) {
    OnAllClosed();
  } else {
    scheduler_.Stop(fast);
  }
  return op;
}

bool Session::WaitStopped(std::chrono::milliseconds timeout) {
  std::unique_lock lock(stop_mutex_);
  return stop_cv_.wait_for(lock, timeout, [this] { return stopped_signalled_; });
}

void Session::OnAllClosed() {
  // Runs on an executor thread. Operation::Succeed wakes the host, which
  // may destroy this Session right away (its ~Scheduler waits for the task
  // we are on, but stop_mutex_ is a member declared after scheduler_ and
  // is torn down first). So: transition, wake stop_cv_ waiters and hand the
  // ops out under the lock, then complete the ops without touching |this|.
  std::vector<OperationId> ops;
  OperationRegistry* registry = &operations_;
  {
    std::lock_guard lock(stop_mutex_);
    if (stopped_signalled_) return;
    ops.swap(stop_waiters_);
    // State first so a WaitStopped/WaitOp waker observes kStopped; then the
    // flag: late Stop() callers see it and succeed their op inline.
    if (state() != SessionState::kFailed) Transition(SessionState::kStopped);
    stopped_signalled_ = true;
    stop_cv_.notify_all();
  }
  for (const OperationId op : ops) registry->Succeed(op);
}

// REL-1/REL-2 first cut (retries with P5): a failed node fails the session,
// which fast-stops the graph. Runs on the executor thread of the failure.
void Session::OnNodeFailed(NodeRuntime& node, const Status& status) {
  if (events_.on_node_failed) events_.on_node_failed(node, status);
  if (!options_.fail_session_on_node_failure) return;
  SessionState from = state();
  for (;;) {
    if (from == SessionState::kFailed || from == SessionState::kStopped ||
        from == SessionState::kStopping) {
      return;
    }
    if (TransitionIf(from, SessionState::kFailed)) break;
    from = state();
  }
  failure_ = Status(status.code(),
                    "node '" + node.external_id() + "' failed: " + status.message(), false,
                    status.context_json());
  coordinator_.CancelAll("session failed");
  scheduler_.Stop(true);
}

Result<OperationId> Session::Apply(MutationPatch patch, CallerContext caller) {
  if (!AcceptsMutations()) return NotMutable();
  if (patch.actions.empty()) return Status::InvalidArgument("patch has no actions");
  JsonObject d;
  d["patch"] = GraphSpecParser::ToJson(patch);
  const OperationId op = operations_.Create("mutation.apply", id(), caller, JsonValue(std::move(d)));
  coordinator_.Submit(MutationRequest{op, std::move(patch)});
  return op;
}

Result<DryRunResult> Session::DryRun(const MutationPatch& patch) const {
  if (!AcceptsMutations()) return NotMutable();
  return coordinator_.DryRun(*current_topology(), patch);
}

Result<ParameterUpdate> Session::SetParameters(std::string_view node_id, JsonValue parameters,
                                               CallerContext caller) {
  if (!AcceptsMutations()) return NotMutable();
  const std::shared_ptr<RuntimeTopology> topo = current_topology();
  NodeRuntime* node = topo->FindNode(node_id);
  if (node == nullptr) return Status::NotFound("node '" + std::string(node_id) + "' not found");
  if (!parameters.is_object() || parameters.as_object().empty()) {
    return Status::InvalidArgument("parameters must be a non-empty JSON object");
  }
  const OperationId op = operations_.Create(
      "parameter.set", id(), std::move(caller),
      JsonValue(JsonObject{{"node", JsonValue(std::string(node_id))}, {"parameters", parameters}}));
  auto v = node->parameters().Submit(ParameterUpdateRequest{op, std::move(parameters)},
                                     node->capability().parameters.hot_updatable);
  if (!v.ok()) {
    operations_.Fail(op, v.status());
    return v.status();
  }
  // Fully validated at submit; takes effect at the next packet boundary
  // (12 §5). CTL-5: synchronous result carries the version.
  operations_.Succeed(op, topo->version(), *v);
  return ParameterUpdate{*v, op};
}

JsonValue Session::Snapshot() const {
  const std::shared_ptr<RuntimeTopology> topo = current_topology();
  JsonObject o;
  o["session_id"] = JsonValue(id());
  o["state"] = JsonValue(ToString(state()));
  o["topology_version"] = JsonValue(topo->version());
  o["graph"] = GraphSpecParser::ToJson(topo->spec());
  JsonArray nodes;
  for (const NodeRuntimeRef& n : topo->nodes()) {
    JsonObject nj;
    nj["id"] = JsonValue(n->external_id());
    nj["node_id"] = JsonValue(n->id());
    nj["op"] = JsonValue(n->operator_key().ToString());
    nj["state"] = JsonValue(ToString(n->state()));
    const auto params = n->parameters().Current();
    nj["parameter_version"] = JsonValue(params->version);
    nj["parameters"] = params->values;
    JsonObject m;
    m["invocations"] = JsonValue(n->metrics().invocations.load(std::memory_order_relaxed));
    m["packets_in"] = JsonValue(n->metrics().packets_in.load(std::memory_order_relaxed));
    m["packets_out"] = JsonValue(n->metrics().packets_out.load(std::memory_order_relaxed));
    m["errors"] = JsonValue(n->metrics().errors.load(std::memory_order_relaxed));
    if (n->is_async()) {
      const NodeMetrics& nm = n->metrics();
      JsonObject a;
      a["in_flight"] = JsonValue(static_cast<std::uint64_t>(n->async_pending()));
      a["submitted"] = JsonValue(nm.submitted.load(std::memory_order_relaxed));
      a["completed"] = JsonValue(nm.completed.load(std::memory_order_relaxed));
      a["wait_p50_ns"] = JsonValue(nm.async_wait.P50());
      a["wait_p99_ns"] = JsonValue(nm.async_wait.P99());
      a["wait_count"] = JsonValue(nm.async_wait.count());
      a["batch_count"] = JsonValue(nm.batch_count.load(std::memory_order_relaxed));
      a["batch_size_sum"] = JsonValue(nm.batch_size_sum.load(std::memory_order_relaxed));
      a["reorder_gaps"] = JsonValue(nm.reorder_gaps.load(std::memory_order_relaxed));
      a["late_completions"] = JsonValue(nm.late_completions.load(std::memory_order_relaxed));
      a["orphan_completions"] = JsonValue(nm.orphan_completions.load(std::memory_order_relaxed));
      m["async"] = JsonValue(std::move(a));
    }
    nj["metrics"] = JsonValue(std::move(m));
    nodes.emplace_back(std::move(nj));
  }
  o["nodes"] = JsonValue(std::move(nodes));
  JsonArray edges;
  for (const EdgeChannelRef& e : topo->edges()) {
    JsonObject ej;
    ej["id"] = JsonValue(e->external_id());
    ej["from"] = JsonValue(e->from().ToString());
    ej["to"] = JsonValue(e->to().ToString());
    ej["contract"] = e->contract().ToJson();
    const auto m = e->metrics().Load();
    ej["queue_depth"] = JsonValue(static_cast<std::uint64_t>(m.queue_depth));
    ej["max_depth"] = JsonValue(static_cast<std::uint64_t>(m.max_depth));
    ej["pushed"] = JsonValue(m.pushed);
    ej["popped"] = JsonValue(m.popped);
    ej["drop_count"] = JsonValue(m.drop_count);
    edges.emplace_back(std::move(ej));
  }
  o["edges"] = JsonValue(std::move(edges));
  o["pending_retirements"] = JsonValue(static_cast<std::uint64_t>(scheduler_.pending_retirements()));
  return JsonValue(std::move(o));
}

}  // namespace ge
