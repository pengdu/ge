#ifndef GE_INFER_OPERATORS_H_
#define GE_INFER_OPERATORS_H_

// ge_infer: builtin operators for inference pipelines (OD-09 scene library,
// depends on ge_core only). Register them on a BuiltinOperatorFactory and
// hand that to EngineConfig::builtin_operators.
//
//   FlowLimiter@1.0.0   in(VideoFrame) -> out(VideoFrame); finished(Json) feedback
//   OnnxInfer@1.0.0     in(Tensor) -> out(Tensor); async, see onnx.h
//
// slow consumer. It lets at most |max_in_flight| packets past; the consumer
// (or anything downstream of it) reports completion on the |finished| input
// through a feedback edge, which frees a slot. A packet arriving while every
// slot is taken is dropped (on_limit=drop, default) or parked in a bounded
// buffer and released on the next completion (on_limit=hold). Compared with
// queue backpressure this bounds *latency* rather than queue depth (SLA-1):
// the newest frame is what gets through, not the oldest.
//
//   options:
//     max_in_flight   uint >= 1, default 1      (hot-updatable)
//     on_limit        "drop" | "hold", default "drop"
//     max_held        uint >= 1, default max_in_flight (hold only)
//     timeout_ms      uint, default 0 = never; an in-flight slot whose
//                     completion has not arrived within timeout_ms is
//                     reclaimed (guards against consumers that drop frames)
//   matching:
//     A completion packet releases every in-flight entry whose pts_ns is
//     <= its own pts_ns (the consumer may coalesce); a completion with
//     pts_ns == 0 releases the oldest entry.
//   events (ProcessRequest::events):
//     flow_limiter.drop     kDebug   {seq, pts_ns, in_flight}
//     flow_limiter.timeout  kWarning {pts_ns, age_ms}
//
// The |finished| port is optional on purpose: a required port would make the
// limiter wait for the consumer's EOS while the consumer waits for the
// Sync policy defaults to "latest": every |in| trigger also carries the
// newest |finished| packet (older completions are subsumed by the pts <=
// rule), so a saturated input queue cannot starve the feedback. Under
// "hold" a parked frame is therefore released on the next |in| trigger or
// at flush, not the instant the completion lands. "any" is accepted too but
// reads |finished| only while |in| is empty.
//
// Type tags: negotiation matches type_tag exactly (CAP-7), there is no
// type-agnostic port yet (OD-11). One registered key therefore serves one
// (in, finished) tag pair; register further pairs under their own keys.

#include <memory>
#include <string_view>

#include <ge/cpp/capability.h>
#include <ge/cpp/operator.h>

namespace ge::infer {

inline constexpr std::string_view kOpFlowLimiter = "FlowLimiter@1.0.0";
inline constexpr std::string_view kEventFlowLimiterDrop = "flow_limiter.drop";
inline constexpr std::string_view kEventFlowLimiterTimeout = "flow_limiter.timeout";

[[nodiscard]] CapabilityDescriptor FlowLimiterCapability(OperatorKey key = *OperatorKey::Parse(kOpFlowLimiter),
                                                         std::string_view in_tag = "VideoFrame",
                                                         std::string_view finished_tag = "Json");
[[nodiscard]] std::unique_ptr<Operator> MakeFlowLimiter(const OperatorCreateArgs& args);

// Registers FlowLimiter for one (in, finished) tag pair under |key|.
void RegisterFlowLimiter(BuiltinOperatorFactory& factory, std::string_view in_tag = "VideoFrame",
                         std::string_view finished_tag = "Json",
                         OperatorKey key = *OperatorKey::Parse(kOpFlowLimiter));

// Registers every operator above with its default tags.
void RegisterInferOperators(BuiltinOperatorFactory& factory);
[[nodiscard]] std::shared_ptr<BuiltinOperatorFactory> MakeInferOperatorFactory();

}  // namespace ge::infer

#endif
