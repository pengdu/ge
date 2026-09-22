#include <ge/media/transcode_controller.h>

#include <algorithm>
#include <thread>

#include <ge/media/operators.h>

namespace ge::media {

TranscodeController::TranscodeController(Engine& engine, Session& session, RenditionTemplate::BaseOptions base,
                                         std::vector<RenditionSpec> initial)
    : engine_(engine), session_(session), base_(std::move(base)) {
  for (RenditionSpec& r : initial) renditions_.emplace(r.id, std::move(r));
  EventFilter filter;
  filter.session = session_.id();
  filter.types = {"drain_timeout", std::string(kEventMediaFormatChanged), "node_failed"};
  subscription_ = engine_.events().Subscribe(std::move(filter), [this](const Event& e) {
    if (e.type == "drain_timeout") {
      drain_timeouts_.fetch_add(1, std::memory_order_relaxed);
    } else if (e.type == kEventMediaFormatChanged) {
      format_changes_.fetch_add(1, std::memory_order_relaxed);
    } else if (e.type == "node_failed") {
      node_failures_.fetch_add(1, std::memory_order_relaxed);
    }
  });
}

TranscodeController::~TranscodeController() { engine_.events().Cancel(subscription_); }

std::optional<RenditionSpec> TranscodeController::Lookup(std::string_view id) const {
  const auto it = renditions_.find(std::string(id));
  if (it == renditions_.end()) return std::nullopt;
  return it->second;
}

Result<OperationId> TranscodeController::AddRendition(const RenditionSpec& spec) {
  Result<MutationPatch> patch = RenditionTemplate::AddRendition(spec, base_);
  if (!patch.ok()) return patch.status();
  std::lock_guard lock(mutex_);
  if (renditions_.contains(spec.id)) return Status::AlreadyExists("rendition '" + spec.id + "' exists");
  Result<OperationId> op = session_.Apply(std::move(*patch));
  if (!op.ok()) return op;
  renditions_.emplace(spec.id, spec);
  return op;
}

Result<OperationId> TranscodeController::RemoveRendition(std::string_view id, bool drain) {
  std::lock_guard lock(mutex_);
  const std::optional<RenditionSpec> spec = Lookup(id);
  if (!spec) return Status::NotFound("rendition '" + std::string(id) + "' not found");
  const RemovePolicy policy = drain ? RemovePolicy::kDrain : RemovePolicy::kFast;
  Result<OperationId> op = session_.Apply(RenditionTemplate::RemoveRendition(id, policy, spec->audio, base_));
  if (!op.ok()) return op;
  renditions_.erase(std::string(id));
  filters_.erase(std::string(id));
  pending_removals_.emplace(*op, std::make_pair(std::string(id), drain));
  return op;
}

Result<OperationId> TranscodeController::InsertFilter(std::string_view id, const FilterSpec& filter) {
  std::lock_guard lock(mutex_);
  if (!Lookup(id)) return Status::NotFound("rendition '" + std::string(id) + "' not found");
  std::vector<FilterSpec>& filters = filters_[std::string(id)];
  Result<MutationPatch> patch = RenditionTemplate::InsertFilter(session_.current_topology()->spec(), id, filter);
  if (!patch.ok()) return patch.status();
  Result<OperationId> op = session_.Apply(std::move(*patch));
  if (!op.ok()) return op;
  filters.push_back(filter);
  return op;
}

Result<OperationId> TranscodeController::RemoveFilter(std::string_view id, std::string_view filter_id, bool drain) {
  std::lock_guard lock(mutex_);
  if (!Lookup(id)) return Status::NotFound("rendition '" + std::string(id) + "' not found");
  std::vector<FilterSpec>& filters = filters_[std::string(id)];
  const auto it = std::find_if(filters.begin(), filters.end(), [&](const FilterSpec& f) { return f.id == filter_id; });
  if (it == filters.end()) {
    return Status::NotFound("filter '" + std::string(filter_id) + "' not found in rendition '" + std::string(id) + "'");
  }
  const RemovePolicy policy = drain ? RemovePolicy::kDrain : RemovePolicy::kFast;
  Result<OperationId> op = session_.Apply(RenditionTemplate::RemoveFilter(id, filter_id, policy));
  if (!op.ok()) return op;
  filters.erase(it);
  return op;
}

Result<ParameterUpdate> TranscodeController::UpdateFilter(std::string_view id, std::string_view filter_id,
                                                          std::string_view chain) {
  std::lock_guard lock(mutex_);
  if (!Lookup(id)) return Status::NotFound("rendition '" + std::string(id) + "' not found");
  std::vector<FilterSpec>& filters = filters_[std::string(id)];
  const auto it = std::find_if(filters.begin(), filters.end(), [&](const FilterSpec& f) { return f.id == filter_id; });
  if (it == filters.end()) {
    return Status::NotFound("filter '" + std::string(filter_id) + "' not found in rendition '" + std::string(id) + "'");
  }
  if (Status s = ValidateFilterChain(chain); !s.ok()) return s;
  Result<ParameterUpdate> r =
      session_.SetParameters(RenditionTemplate::Ids(id).Filter(filter_id), RenditionTemplate::FilterParameters(chain));
  if (!r.ok()) return r;
  it->chain = std::string(chain);
  return r;
}

std::vector<FilterSpec> TranscodeController::Filters(std::string_view id) const {
  std::lock_guard lock(mutex_);
  const auto it = filters_.find(std::string(id));
  return it == filters_.end() ? std::vector<FilterSpec>{} : it->second;
}

Result<ParameterUpdate> TranscodeController::UpdateRendition(std::string_view id, const RenditionTemplate::HotUpdate& u) {
  std::lock_guard lock(mutex_);
  const auto it = renditions_.find(std::string(id));
  if (it == renditions_.end()) return Status::NotFound("rendition '" + std::string(id) + "' not found");
  const RenditionNodeIds ids = RenditionTemplate::Ids(id);
  std::optional<ParameterUpdate> last;
  const JsonValue enc = RenditionTemplate::EncoderParameters(u);
  if (!enc.as_object().empty()) {
    Result<ParameterUpdate> r = session_.SetParameters(ids.venc, enc);
    if (!r.ok()) return r;
    last = *r;
  }
  const JsonValue scale = RenditionTemplate::ScaleParameters(u);
  if (!scale.as_object().empty()) {
    Result<ParameterUpdate> r = session_.SetParameters(ids.scale, scale);
    if (!r.ok()) return r;
    last = *r;
  }
  if (!last) return Status::InvalidArgument("no hot-updatable field set");
  if (u.bitrate_kbps) it->second.bitrate_kbps = *u.bitrate_kbps;
  if (u.gop) it->second.gop = *u.gop;
  if (u.watermark) it->second.watermark = *u.watermark;
  if (u.clear_watermark) it->second.watermark.reset();
  return *last;
}

Result<ParameterUpdate> TranscodeController::RequestKeyFrame(std::string_view id) {
  RenditionTemplate::HotUpdate u;
  u.force_idr = true;
  return UpdateRendition(id, u);
}

Result<OperationId> TranscodeController::SwitchCodec(std::string_view id, const RenditionSpec& replacement) {
  if (replacement.id == id) return Status::InvalidArgument("replacement must use a new rendition id");
  Result<MutationPatch> add = RenditionTemplate::AddRendition(replacement, base_);
  if (!add.ok()) return add.status();
  std::lock_guard lock(mutex_);
  const std::optional<RenditionSpec> old = Lookup(id);
  if (!old) return Status::NotFound("rendition '" + std::string(id) + "' not found");
  if (renditions_.contains(replacement.id)) return Status::AlreadyExists("rendition '" + replacement.id + "' exists");
  MutationPatch patch = RenditionTemplate::RemoveRendition(id, RemovePolicy::kDrain, old->audio, base_);
  for (MutationAction& a : add->actions) patch.actions.push_back(std::move(a));
  Result<OperationId> op = session_.Apply(std::move(patch));
  if (!op.ok()) return op;
  renditions_.erase(std::string(id));
  filters_.erase(std::string(id));
  renditions_.emplace(replacement.id, replacement);
  pending_removals_.emplace(*op, std::make_pair(std::string(id), true));
  return op;
}

void TranscodeController::Pump() {
  // Inline engine: one executor task per pass so a Wait() does not run the
  // whole remaining input (callers interleave their own control actions).
  if (engine_.config().cpu_threads == 0) {
    (void)session_.PumpMutations();
    if (!engine_.executor().RunOne()) engine_.Tick();
    session_.Tick();
    return;
  }
  if (!engine_.config().watchdog_thread) engine_.Tick();
}

Result<OperationRecord> TranscodeController::Wait(OperationId op, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const bool inline_engine = engine_.config().cpu_threads == 0 || !engine_.config().watchdog_thread;
  for (;;) {
    Pump();
    const std::chrono::milliseconds slice = inline_engine ? std::chrono::milliseconds(0) : timeout;
    if (std::optional<OperationRecord> r = engine_.operations().Wait(op, slice)) {
      std::lock_guard lock(mutex_);
      pending_removals_.erase(op);
      return *r;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return Status::Internal("operation " + std::to_string(op) + " did not finish within " +
                              std::to_string(timeout.count()) + "ms");
    }
    if (!inline_engine) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::vector<RenditionSpec> TranscodeController::Renditions() const {
  std::lock_guard lock(mutex_);
  std::vector<RenditionSpec> out;
  out.reserve(renditions_.size());
  for (const auto& [id, r] : renditions_) out.push_back(r);
  return out;
}

std::optional<RenditionSpec> TranscodeController::Find(std::string_view id) const {
  std::lock_guard lock(mutex_);
  return Lookup(id);
}

TranscodeStats TranscodeController::stats() const {
  TranscodeStats s;
  s.drain_timeouts = drain_timeouts_.load(std::memory_order_relaxed);
  s.format_changes = format_changes_.load(std::memory_order_relaxed);
  s.node_failures = node_failures_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace ge::media
