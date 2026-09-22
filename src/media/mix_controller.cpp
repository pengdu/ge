#include <ge/media/mix_controller.h>

#include <algorithm>
#include <thread>

namespace ge::media {

MixController::MixController(Engine& engine, Session& session, LayoutSpec layout, MixOptions options,
                             std::vector<MixSpec> initial)
    : engine_(engine), session_(session), layout_(std::move(layout)), options_(std::move(options)) {
  for (std::size_t i = 0; i < initial.size(); ++i) {
    ports_.emplace(initial[i].member_id, static_cast<int>(i));
    members_.push_back(std::move(initial[i]));
  }
}

MixController::~MixController() = default;

int MixController::FreePortLocked() const {
  for (int i = 0; i < MixTemplate::kMaxMembers; ++i) {
    const bool used = std::any_of(ports_.begin(), ports_.end(), [&](const auto& kv) { return kv.second == i; });
    if (!used) return i;
  }
  return -1;
}

// The port a member occupies is fixed when it joins (it is the compose/amix
// input it was wired to), so the sender records it alongside the layout.
LayoutSpec MixController::ResolvedLocked() const {
  LayoutSpec out = MixTemplate::EffectiveLayout(layout_, members_);
  std::vector<SlotSpec> manual = layout_.slots;  // explicit host overrides
  for (SlotSpec& s : out.slots) {
    for (const SlotSpec& m : manual) {
      if (m.member_id != s.member_id) continue;
      if (m.rect.w > 0 && m.rect.h > 0) s.rect = m.rect;
      if (m.has_gain) {
        s.gain_millis = m.gain_millis;
        s.has_gain = true;
      }
    }
    const auto it = ports_.find(s.member_id);
    if (it != ports_.end()) s.port = MixTemplate::MemberPort(it->second);
  }
  return out;
}

Status MixController::PushLayoutLocked() {
  Result<ParameterUpdate> r =
      session_.SetParameters(MixNodeIds{}.compose, MixTemplate::ComposeHotParameters(ResolvedLocked(), options_));
  if (!r.ok()) return r.status();
  return Status::Ok();
}

Result<ParameterUpdate> MixController::PushGainsLocked() {
  JsonArray arr;
  for (const SlotSpec& s : ResolvedLocked().slots) {
    if (s.port.empty()) continue;
    arr.push_back(JsonValue(JsonObject{{"member", JsonValue(s.port)},
                                       {"gain_millis", JsonValue(s.has_gain ? s.gain_millis : 1000)}}));
  }
  return session_.SetParameters(MixNodeIds{}.amix, JsonValue(JsonObject{{"gains", JsonValue(std::move(arr))}}));
}

Result<OperationId> MixController::AddMember(const MixSpec& spec) {
  std::lock_guard lock(mutex_);
  if (ports_.contains(spec.member_id)) return Status::AlreadyExists("member '" + spec.member_id + "' exists");
  const int port = FreePortLocked();
  if (port < 0) return Status::ResourceExhausted("all member slots are occupied");
  Result<MutationPatch> patch = MixTemplate::AddMember(spec, port, layout_, options_);
  if (!patch.ok()) return patch.status();
  Result<OperationId> op = session_.Apply(std::move(*patch));
  if (!op.ok()) return op;
  ports_.emplace(spec.member_id, port);
  members_.push_back(spec);
  // The layout follows the membership at once (MX-I-1: the join and the
  // re-grid are one host action); the compose node applies it on the next
  // frame, before the new branch delivers its first picture.
  if (Status s = PushLayoutLocked(); !s.ok()) return s;
  if (options_.audio) {
    if (Result<ParameterUpdate> g = PushGainsLocked(); !g.ok()) return g.status();
  }
  return op;
}

Result<OperationId> MixController::RemoveMember(std::string_view member_id, bool drain) {
  std::lock_guard lock(mutex_);
  const auto it = ports_.find(std::string(member_id));
  if (it == ports_.end()) return Status::NotFound("member '" + std::string(member_id) + "' not found");
  MixSpec spec;
  for (const MixSpec& m : members_) {
    if (m.member_id == member_id) spec = m;
  }
  const RemovePolicy policy = drain ? RemovePolicy::kDrain : RemovePolicy::kFast;
  Result<MutationPatch> patch = MixTemplate::RemoveMember(spec, it->second, options_, policy, drain);
  if (!patch.ok()) return patch.status();
  Result<OperationId> op = session_.Apply(std::move(*patch));
  if (!op.ok()) return op;
  ports_.erase(it);
  std::erase_if(members_, [&](const MixSpec& m) { return m.member_id == member_id; });
  if (Status s = PushLayoutLocked(); !s.ok()) return s;
  return op;
}

Result<ParameterUpdate> MixController::SetLayout(const LayoutSpec& layout) {
  std::lock_guard lock(mutex_);
  if (layout.width != layout_.width || layout.height != layout_.height || layout.fps != layout_.fps) {
    return Status::InvalidArgument("canvas size/fps are structural; rebuild the session to change them");
  }
  layout_ = layout;
  return session_.SetParameters(MixNodeIds{}.compose, MixTemplate::ComposeHotParameters(ResolvedLocked(), options_));
}

Result<ParameterUpdate> MixController::MoveMember(std::string_view member_id, const MixRect& rect,
                                                  std::optional<MixFit> fit, std::optional<int> z,
                                                  std::optional<bool> visible) {
  std::lock_guard lock(mutex_);
  if (!ports_.contains(std::string(member_id))) {
    return Status::NotFound("member '" + std::string(member_id) + "' not found");
  }
  if (rect.w <= 0 || rect.h <= 0) return Status::InvalidArgument("rect must have positive w/h");
  SlotSpec* slot = nullptr;
  for (SlotSpec& s : layout_.slots) {
    if (s.member_id == member_id) slot = &s;
  }
  if (slot == nullptr) {
    SlotSpec s;
    s.member_id = std::string(member_id);
    layout_.slots.push_back(std::move(s));
    slot = &layout_.slots.back();
  }
  slot->rect = rect;
  if (fit) slot->fit = *fit;
  if (z) slot->z = *z;
  if (visible) slot->visible = *visible;
  return session_.SetParameters(MixNodeIds{}.compose, MixTemplate::ComposeHotParameters(ResolvedLocked(), options_));
}

Result<ParameterUpdate> MixController::SetGain(std::string_view member_id, std::int64_t gain_millis) {
  std::lock_guard lock(mutex_);
  if (!options_.audio) return Status::InvalidArgument("session has no audio mix");
  if (gain_millis < 0 || gain_millis > 10000) return Status::InvalidArgument("gain_millis must be within 0..10000");
  const auto it = ports_.find(std::string(member_id));
  if (it == ports_.end()) return Status::NotFound("member '" + std::string(member_id) + "' not found");
  for (SlotSpec& s : layout_.slots) {
    if (s.member_id == member_id) {
      s.gain_millis = gain_millis;
      s.has_gain = true;
    }
  }
  bool found = false;
  for (const SlotSpec& s : layout_.slots) {
    if (s.member_id == member_id) found = true;
  }
  if (!found) {
    SlotSpec s;
    s.member_id = std::string(member_id);
    s.gain_millis = gain_millis;
    s.has_gain = true;
    layout_.slots.push_back(std::move(s));
  }
  JsonArray arr;
  arr.push_back(JsonValue(JsonObject{{"member", JsonValue(MixTemplate::MemberPort(it->second))},
                                     {"gain_millis", JsonValue(gain_millis)}}));
  return session_.SetParameters(MixNodeIds{}.amix, JsonValue(JsonObject{{"gains", JsonValue(std::move(arr))}}));
}

void MixController::Pump() {
  if (engine_.config().cpu_threads == 0) {
    (void)session_.PumpMutations();
    if (!engine_.executor().RunOne()) engine_.Tick();
    session_.Tick();
    return;
  }
  if (!engine_.config().watchdog_thread) engine_.Tick();
}

Result<OperationRecord> MixController::Wait(OperationId op, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const bool inline_engine = engine_.config().cpu_threads == 0 || !engine_.config().watchdog_thread;
  for (;;) {
    Pump();
    const std::chrono::milliseconds slice = inline_engine ? std::chrono::milliseconds(0) : timeout;
    if (std::optional<OperationRecord> r = engine_.operations().Wait(op, slice)) return *r;
    if (std::chrono::steady_clock::now() >= deadline) {
      return Status::Internal("operation " + std::to_string(op) + " did not finish within " +
                              std::to_string(timeout.count()) + "ms");
    }
    if (!inline_engine) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::vector<MixSpec> MixController::Members() const {
  std::lock_guard lock(mutex_);
  return members_;
}

LayoutSpec MixController::Layout() const {
  std::lock_guard lock(mutex_);
  return layout_;
}

std::string MixController::PortOf(std::string_view member_id) const {
  std::lock_guard lock(mutex_);
  const auto it = ports_.find(std::string(member_id));
  return it == ports_.end() ? std::string() : MixTemplate::MemberPort(it->second);
}

}  // namespace ge::media
