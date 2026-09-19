#ifndef GE_INFER_ONNX_H_
#define GE_INFER_ONNX_H_

// OnnxInfer@1.0.0: asynchronous ONNX Runtime inference operator (12 §3.5 /
// §4.2, ASY-1..8). First real backend behind AsyncRuntime.
//
//   in(Tensor)  -> out(Tensor)
//
// options:
//   model            path to .onnx (required); also the Batcher key (ASY-6)
//   input_name       override the model's first input name
//   output_name      override the model's first output name
//   intra_threads    ORT intra-op threads, default 1 (OD-07: parallelism by
//                    many single-threaded instances, not one wide one)
//   workers          backend threads that call Ort::Run, default 1
//   provider         "cpu" (default) | "coreml" (macOS, best effort)
//   simulate_delay_ms  test hook: sleep before Run
//
// Execution model: Submit() enqueues the request on the operator's worker
// queue and returns immediately (ASY-1). Members of one AsyncRuntime batch
// (same batch_id, submitted back to back on one thread) are stacked along
// the model's leading dimension and run as a single Ort::Run; each member
// gets its own CompletionEvent with the matching slice (ASY-6). A member
// whose tensor shape does not match the batch leader is run on its own.
// Close() waits for the worker to drain (so the CompletionSink outlives every
// callback, per 13 §4.6); fast_shutdown drops queued requests with
// CANCELLED completions instead of running them.
//
// The Tensor packet convention is in tensor.h.

#include <memory>
#include <string_view>

#include <ge/cpp/capability.h>
#include <ge/cpp/operator.h>

namespace ge::infer {

inline constexpr std::string_view kOpOnnxInfer = "OnnxInfer@1.0.0";

[[nodiscard]] CapabilityDescriptor OnnxInferCapability();
[[nodiscard]] std::unique_ptr<Operator> MakeOnnxInfer(const OperatorCreateArgs& args);
void RegisterOnnxInfer(BuiltinOperatorFactory& factory);

// Build-time probe so tests can skip when ge_infer was built without ORT.
[[nodiscard]] bool OnnxRuntimeAvailable() noexcept;
[[nodiscard]] std::string_view OnnxRuntimeVersion() noexcept;

}  // namespace ge::infer

#endif
