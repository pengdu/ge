// FlowLimiter: max_in_flight admission with a completion feedback edge
// (include/ge/infer/operators.h).
#include <ge/infer/operators.h>

#include <ge/infer/onnx.h>

#include <chrono>
#include <deque>
#include <string>

namespace ge::infer {

namespace {

enum class OnLimit : std::uint8_t { kDrop, kHold };

struct InFlight {
  std::int64_t pts_ns = 0;
  PacketSeq seq = 0;
  std::chrono::steady_clock::time_point since;
};

class FlowLimiter final : public Operator {
 public:
  explicit FlowLimiter(JsonValue options) : options_(std::move(options)) {}

  Status Open(const OpenRequest& r) override {
    external_id_ = std::string(r.external_id);
    const std::int64_t max_in_flight = options_.GetInteger("max_in_flight").value_or(1);
    if (max_in_flight < 1) return Status::InvalidArgument(Prefix() + "max_in_flight must be >= 1");
    max_in_flight_ = static_cast<std::size_t>(max_in_flight);
    const std::string on_limit = options_.GetString("on_limit").value_or("drop");
    if (on_limit == "drop") {
      on_limit_ = OnLimit::kDrop;
    } else if (on_limit == "hold") {
      on_limit_ = OnLimit::kHold;
    } else {
      return Status::InvalidArgument(Prefix() + "on_limit must be \"drop\" or \"hold\"");
    }
    const std::int64_t max_held = options_.GetInteger("max_held").value_or(max_in_flight);
    if (max_held < 1) return Status::InvalidArgument(Prefix() + "max_held must be >= 1");
    max_held_ = static_cast<std::size_t>(max_held);
    const std::int64_t timeout_ms = options_.GetInteger("timeout_ms").value_or(0);
    if (timeout_ms < 0) return Status::InvalidArgument(Prefix() + "timeout_ms must be >= 0");
    timeout_ = std::chrono::milliseconds(timeout_ms);
    return Status::Ok();
  }

  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.parameters != nullptr) {
      if (const auto m = req.parameters->GetInteger("max_in_flight"); m && *m >= 1) {
        max_in_flight_ = static_cast<std::size_t>(*m);
      }
    }
    ReclaimTimedOut(req);
    if (req.flags & GE_PROCESS_FLAG_FLUSH) {
      // Input ended: whatever is held goes out; the consumer's queue takes
      // the burst (it is bounded by max_held).
      while (!held_.empty()) {
        if (Status s = Admit(req, std::move(held_.front())); !s.ok()) return s;
        held_.pop_front();
      }
      return ProcessResult::kContinue;
    }
    // Completions first regardless of batch order: a "latest" batch pairs
    // the trigger frame with the newest completion, and that completion
    // must count before the frame is judged.
    for (std::size_t i = 0; i < req.inputs.size(); ++i) {
      if (req.input_ports[i] == "finished" && !req.inputs[i]->is_event()) Release(*req.inputs[i]);
    }
    if (Status s = ReleaseHeld(req); !s.ok()) return s;
    for (std::size_t i = 0; i < req.inputs.size(); ++i) {
      const PacketRef& p = req.inputs[i];
      if (p->is_event() || req.input_ports[i] != "in") continue;
      if (in_flight_.size() < max_in_flight_) {
        if (Status s = Admit(req, p); !s.ok()) return s;
        continue;
      }
      if (on_limit_ == OnLimit::kHold && held_.size() < max_held_) {
        held_.push_back(p);
        continue;
      }
      // Newest-wins: under hold the oldest held frame is what becomes stale.
      if (on_limit_ == OnLimit::kHold) {
        Drop(req, *held_.front());
        held_.pop_front();
        held_.push_back(p);
      } else {
        Drop(req, *p);
      }
    }
    return ProcessResult::kContinue;
  }

  Status Close(const CloseRequest&) override {
    in_flight_.clear();
    held_.clear();
    return Status::Ok();
  }

 private:
  [[nodiscard]] std::string Prefix() const { return "node '" + external_id_ + "': "; }

  Status Admit(const ProcessRequest& req, PacketRef p) {
    in_flight_.push_back({p->header.pts_ns, p->header.seq, std::chrono::steady_clock::now()});
    Packet out = *p;  // shares payload
    Status s = req.sink->Emit("out", std::move(out));
    if (!s.ok() && s.code() != GE_STATUS_WOULD_BLOCK) {
      in_flight_.pop_back();
      return s;
    }
    return Status::Ok();
  }

  Status ReleaseHeld(const ProcessRequest& req) {
    while (!held_.empty() && in_flight_.size() < max_in_flight_) {
      if (Status s = Admit(req, std::move(held_.front())); !s.ok()) return s;
      held_.pop_front();
    }
    return Status::Ok();
  }

  // A completion at pts T frees every entry admitted at pts <= T; pts 0
  // (no timestamp) frees the oldest.
  void Release(const Packet& finished) {
    if (in_flight_.empty()) return;
    if (finished.header.pts_ns == 0) {
      in_flight_.pop_front();
      return;
    }
    while (!in_flight_.empty() && in_flight_.front().pts_ns <= finished.header.pts_ns) in_flight_.pop_front();
  }

  void ReclaimTimedOut(const ProcessRequest& req) {
    if (timeout_.count() <= 0) return;
    const auto now = std::chrono::steady_clock::now();
    while (!in_flight_.empty() && now - in_flight_.front().since > timeout_) {
      const InFlight& f = in_flight_.front();
      if (req.events != nullptr) {
        req.events->Publish(kEventFlowLimiterTimeout, Severity::kWarning,
                            JsonValue(JsonObject{
                                {"pts_ns", JsonValue(f.pts_ns)},
                                {"seq", JsonValue(static_cast<std::int64_t>(f.seq))},
                                {"age_ms", JsonValue(std::chrono::duration_cast<std::chrono::milliseconds>(now - f.since).count())}}));
      }
      ++timeouts_;
      in_flight_.pop_front();
    }
  }

  void Drop(const ProcessRequest& req, const Packet& p) {
    ++dropped_;
    if (req.events != nullptr) {
      req.events->Publish(kEventFlowLimiterDrop, Severity::kDebug,
                          JsonValue(JsonObject{
                              {"seq", JsonValue(static_cast<std::int64_t>(p.header.seq))},
                              {"pts_ns", JsonValue(p.header.pts_ns)},
                              {"in_flight", JsonValue(static_cast<std::int64_t>(in_flight_.size()))}}));
    }
  }

  JsonValue options_;
  std::string external_id_;
  std::size_t max_in_flight_ = 1;
  std::size_t max_held_ = 1;
  OnLimit on_limit_ = OnLimit::kDrop;
  std::chrono::milliseconds timeout_{0};
  std::deque<InFlight> in_flight_;
  std::deque<PacketRef> held_;
  std::uint64_t dropped_ = 0;
  std::uint64_t timeouts_ = 0;
};

PortCapability Port(std::string_view name, PortDirection dir, std::string_view tag, bool required) {
  PortCapability p;
  p.name = std::string(name);
  p.direction = dir;
  p.type_tag = std::string(tag);
  p.required = required;
  return p;
}

}  // namespace

CapabilityDescriptor FlowLimiterCapability(OperatorKey key, std::string_view in_tag, std::string_view finished_tag) {
  CapabilityDescriptor d;
  d.op = std::move(key);
  d.description = "max_in_flight admission control with a completion feedback edge";
  d.inputs = {Port("in", PortDirection::kInput, in_tag, true),
              Port("finished", PortDirection::kInput, finished_tag, false)};
  // latest first (default): every |in| trigger also sees the newest
  // completion, so a saturated input queue cannot starve |finished|.
  d.inputs[0].sync = std::vector<SyncPolicy>{SyncPolicy::kLatest, SyncPolicy::kAny};
  d.inputs[1].sync = std::vector<SyncPolicy>{SyncPolicy::kLatest, SyncPolicy::kAny};
  d.outputs = {Port("out", PortDirection::kOutput, in_tag, true)};
  d.outputs[0].cardinality = PortCardinality::kMulti;
  d.execution.devices = {DeviceKind::kCpu};
  d.execution.stateful = true;
  d.execution.max_parallelism = 1;
  d.parameters.hot_updatable = {"max_in_flight"};
  JsonObject props;
  props.emplace("max_in_flight", JsonValue(JsonObject{{"type", JsonValue("integer")}, {"minimum", JsonValue(1)}}));
  props.emplace("on_limit", JsonValue(JsonObject{{"type", JsonValue("string")},
                                                 {"enum", JsonValue(JsonArray{JsonValue("drop"), JsonValue("hold")})}}));
  props.emplace("max_held", JsonValue(JsonObject{{"type", JsonValue("integer")}, {"minimum", JsonValue(1)}}));
  props.emplace("timeout_ms", JsonValue(JsonObject{{"type", JsonValue("integer")}, {"minimum", JsonValue(0)}}));
  d.parameters.schema = JsonValue(JsonObject{{"type", JsonValue("object")}, {"properties", JsonValue(std::move(props))}});
  d.events.emits = {std::string(kEventFlowLimiterDrop), std::string(kEventFlowLimiterTimeout)};
  return d;
}

std::unique_ptr<Operator> MakeFlowLimiter(const OperatorCreateArgs& args) {
  return std::make_unique<FlowLimiter>(args.options);
}

void RegisterFlowLimiter(BuiltinOperatorFactory& factory, std::string_view in_tag, std::string_view finished_tag,
                         OperatorKey key) {
  factory.Register(FlowLimiterCapability(std::move(key), in_tag, finished_tag), &MakeFlowLimiter);
}

void RegisterInferOperators(BuiltinOperatorFactory& factory) {
  RegisterFlowLimiter(factory);
  RegisterOnnxInfer(factory);
}

std::shared_ptr<BuiltinOperatorFactory> MakeInferOperatorFactory() {
  auto f = std::make_shared<BuiltinOperatorFactory>();
  RegisterInferOperators(*f);
  return f;
}

}  // namespace ge::infer
