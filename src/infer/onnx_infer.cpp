// OnnxInfer: asynchronous ONNX Runtime backend (include/ge/infer/onnx.h).
#include <ge/infer/onnx.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ge/infer/tensor.h>

#if GE_HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace ge::infer {

namespace {

PortCapability TensorPort(std::string_view name, PortDirection dir) {
  PortCapability p;
  p.name = std::string(name);
  p.direction = dir;
  p.type_tag = std::string(kTagTensor);
  p.required = true;
  p.memory.kinds = std::vector<MemoryKind>{MemoryKind::kHost};
  return p;
}

}  // namespace

CapabilityDescriptor OnnxInferCapability() {
  CapabilityDescriptor d;
  d.op = *OperatorKey::Parse(kOpOnnxInfer);
  d.description = "ONNX Runtime inference (async, host tensors)";
  d.inputs = {TensorPort("in", PortDirection::kInput)};
  d.outputs = {TensorPort("out", PortDirection::kOutput)};
  d.outputs[0].cardinality = PortCardinality::kMulti;
  d.execution.devices = {DeviceKind::kCpu};
  d.execution.stateful = true;
  d.execution.async = true;
  d.execution.max_parallelism = 1;
  d.execution.max_inference_ms = 2000;
  JsonObject props;
  props.emplace("model", JsonValue(JsonObject{{"type", JsonValue("string")}}));
  props.emplace("input_name", JsonValue(JsonObject{{"type", JsonValue("string")}}));
  props.emplace("output_name", JsonValue(JsonObject{{"type", JsonValue("string")}}));
  props.emplace("intra_threads", JsonValue(JsonObject{{"type", JsonValue("integer")}, {"minimum", JsonValue(1)}}));
  props.emplace("workers", JsonValue(JsonObject{{"type", JsonValue("integer")}, {"minimum", JsonValue(1)}}));
  props.emplace("provider", JsonValue(JsonObject{{"type", JsonValue("string")},
                                                 {"enum", JsonValue(JsonArray{JsonValue("cpu"), JsonValue("coreml")})}}));
  props.emplace("simulate_delay_ms", JsonValue(JsonObject{{"type", JsonValue("integer")}, {"minimum", JsonValue(0)}}));
  d.parameters.schema = JsonValue(JsonObject{{"type", JsonValue("object")},
                                             {"properties", JsonValue(std::move(props))},
                                             {"required", JsonValue(JsonArray{JsonValue("model")})}});
  d.resources.amounts["cpu_threads"] = 1;
  return d;
}

#if GE_HAVE_ONNXRUNTIME

namespace {

// One queued Submit. |inputs| keeps the packets (and their payloads) alive
// until the completion is pushed.
struct Job {
  SubmitRequest req;  // input_ports/parameters are not used after enqueue
  std::vector<PacketRef> inputs;
  CompletionSink* sink = nullptr;
  std::uint64_t batch_id = 0;
  std::uint32_t batch_size = 1;
};

DType FromOrt(ONNXTensorElementDataType t, bool* ok) {
  *ok = true;
  switch (t) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: return DType::kFloat32;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8: return DType::kUint8;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: return DType::kInt64;
    default:
      *ok = false;
      return DType::kFloat32;
  }
}

ONNXTensorElementDataType ToOrt(DType d) {
  switch (d) {
    case DType::kFloat32: return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    case DType::kUint8: return ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
    case DType::kInt64: return ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
  }
  return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
}

class OnnxInfer final : public Operator {
 public:
  explicit OnnxInfer(JsonValue options) : options_(std::move(options)) {}
  ~OnnxInfer() override { StopWorkers(true); }

  Status Open(const OpenRequest& r) override {
    external_id_ = std::string(r.external_id);
    const auto model = options_.GetString("model");
    if (!model || model->empty()) return Status::InvalidArgument(Prefix() + "option 'model' is required");
    const std::int64_t intra = options_.GetInteger("intra_threads").value_or(1);
    const std::int64_t workers = options_.GetInteger("workers").value_or(1);
    if (intra < 1 || workers < 1) return Status::InvalidArgument(Prefix() + "intra_threads/workers must be >= 1");
    simulate_delay_ = std::chrono::milliseconds(options_.GetInteger("simulate_delay_ms").value_or(0));
    const std::string provider = options_.GetString("provider").value_or("cpu");
    try {
      env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, ("ge." + external_id_).c_str());
      Ort::SessionOptions so;
      so.SetIntraOpNumThreads(static_cast<int>(intra));
      so.SetInterOpNumThreads(1);
      so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
      if (provider == "coreml") {
#if defined(__APPLE__)
        // Best effort: fall back to CPU if the provider is not compiled in.
        try {
          so.AppendExecutionProvider("CoreML", {});
        } catch (const Ort::Exception&) {
        }
#else
        return Status::InvalidArgument(Prefix() + "provider 'coreml' is only available on macOS");
#endif
      } else if (provider != "cpu") {
        return Status::InvalidArgument(Prefix() + "unknown provider '" + provider + "'");
      }
      session_ = std::make_unique<Ort::Session>(*env_, model->c_str(), so);
      Ort::AllocatorWithDefaultOptions alloc;
      if (session_->GetInputCount() < 1 || session_->GetOutputCount() < 1) {
        return Status::InvalidArgument(Prefix() + "model must have at least one input and one output");
      }
      input_name_ = options_.GetString("input_name").value_or(session_->GetInputNameAllocated(0, alloc).get());
      output_name_ = options_.GetString("output_name").value_or(session_->GetOutputNameAllocated(0, alloc).get());
      // TypeInfo owns the tensor info view: keep it alive while reading.
      const Ort::TypeInfo in_type = session_->GetInputTypeInfo(0);
      const auto in_info = in_type.GetTensorTypeAndShapeInfo();
      bool ok = false;
      input_dtype_ = FromOrt(in_info.GetElementType(), &ok);
      if (!ok) return Status::InvalidArgument(Prefix() + "unsupported model input element type");
      input_dims_ = in_info.GetShape();  // -1 for symbolic
      const Ort::TypeInfo out_type = session_->GetOutputTypeInfo(0);
      const auto out_info = out_type.GetTensorTypeAndShapeInfo();
      output_dtype_ = FromOrt(out_info.GetElementType(), &ok);
      if (!ok) return Status::InvalidArgument(Prefix() + "unsupported model output element type");
      // Leading dim symbolic or 1 => model is batchable along it.
      batchable_ = !input_dims_.empty() && (input_dims_[0] < 0 || input_dims_[0] == 1);
    } catch (const Ort::Exception& e) {
      return Status::InvalidArgument(Prefix() + "onnxruntime: " + e.what());
    }
    pool_ = HostBufferPool::Create();
    stop_ = false;
    fast_ = false;
    for (std::int64_t i = 0; i < workers; ++i) workers_.emplace_back([this] { WorkerLoop(); });
    return Status::Ok();
  }

  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ProcessResult::kContinue;
    return Status::InvalidArgument(Prefix() + "async operator invoked synchronously");
  }

  Status Submit(const SubmitRequest& req) override {
    if (req.completion_sink == nullptr) return Status::InvalidArgument(Prefix() + "no completion sink");
    Job j;
    j.req = req;
    j.req.input_ports.clear();
    j.req.parameters = nullptr;
    j.inputs = req.inputs;
    j.sink = req.completion_sink;
    j.batch_id = req.batch_id;
    j.batch_size = req.batch_size;
    {
      std::lock_guard lock(mutex_);
      if (stop_) return Status::Cancelled(Prefix() + "closing");
      queue_.push_back(std::move(j));
    }
    cv_.notify_one();
    return Status::Ok();
  }

  Status Close(const CloseRequest& r) override {
    StopWorkers(r.fast_shutdown);
    session_.reset();
    env_.reset();
    return Status::Ok();
  }

 private:
  [[nodiscard]] std::string Prefix() const { return "node '" + external_id_ + "': "; }

  void StopWorkers(bool fast) {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
      fast_ = fast;
    }
    cv_.notify_all();
    for (std::thread& t : workers_) {
      if (t.joinable()) t.join();
    }
    workers_.clear();
    // Fast: whatever is still queued is answered CANCELLED so the runtime
    // retires the requests (no orphans, ASY-4).
    std::deque<Job> left;
    {
      std::lock_guard lock(mutex_);
      left.swap(queue_);
    }
    for (Job& j : left) Complete(j, Status::Cancelled(Prefix() + "fast shutdown"), {});
  }

  // Pops the next job plus every queued member of the same batch (they are
  // submitted back to back under the runtime's submit lock, so by the time
  // a worker wakes the whole batch is normally there; a straggler simply
  // runs as its own smaller batch).
  bool NextBatch(std::vector<Job>* out) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
    if (queue_.empty()) return false;  // stop_ and nothing left
    if (stop_ && fast_) return false;  // leave them for StopWorkers to cancel
    Job first = std::move(queue_.front());
    queue_.pop_front();
    const std::uint64_t id = first.batch_id;
    const bool grouped = first.batch_size > 1 && id != 0;
    out->push_back(std::move(first));
    if (grouped) {
      for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->batch_id == id) {
          out->push_back(std::move(*it));
          it = queue_.erase(it);
        } else {
          ++it;
        }
      }
    }
    return true;
  }

  void WorkerLoop() {
    for (;;) {
      std::vector<Job> batch;
      if (!NextBatch(&batch)) return;
      RunBatch(std::move(batch));
    }
  }

  // Validates every member; members whose format differs from the leader
  // are peeled off and run alone. Returns per-member results through
  // Complete().
  void RunBatch(std::vector<Job> jobs) {
    if (simulate_delay_.count() > 0) std::this_thread::sleep_for(simulate_delay_);
    std::vector<TensorFormat> formats(jobs.size());
    std::vector<Job> rest;
    std::vector<Job> same;
    std::optional<TensorFormat> leader;
    for (std::size_t i = 0; i < jobs.size(); ++i) {
      const PacketRef* in = nullptr;
      for (const PacketRef& p : jobs[i].inputs) {
        if (!p->is_event() && !p->is_eos()) {
          in = &p;
          break;
        }
      }
      if (in == nullptr) {
        Complete(jobs[i], Status::InvalidArgument(Prefix() + "submit without a data packet"), {});
        continue;
      }
      auto f = FormatOf(**in);
      if (!f.ok()) {
        Complete(jobs[i], f.status(), {});
        continue;
      }
      if (f->dtype != input_dtype_) {
        Complete(jobs[i], Status::InvalidArgument(Prefix() + "input dtype " + std::string(ToString(f->dtype)) +
                                                  " does not match model " + std::string(ToString(input_dtype_))),
                 {});
        continue;
      }
      // Squeeze a leading 1 so per-sample and batch-of-1 producers agree.
      if (batchable_ && f->shape.size() == input_dims_.size() && f->shape.front() == 1) f->shape.erase(f->shape.begin());
      if (!leader) {
        leader = *f;
        same.push_back(std::move(jobs[i]));
      } else if (*f == *leader) {
        same.push_back(std::move(jobs[i]));
      } else {
        rest.push_back(std::move(jobs[i]));
      }
    }
    if (leader) RunSame(std::move(same), *leader);
    for (Job& j : rest) {
      std::vector<Job> one;
      one.push_back(std::move(j));
      RunBatch(std::move(one));
    }
  }

  void RunSame(std::vector<Job> jobs, const TensorFormat& sample) {
    const std::size_t n = batchable_ ? jobs.size() : 1;
    if (!batchable_ && jobs.size() > 1) {
      // Model has a fixed leading dim: run members one by one.
      for (Job& j : jobs) {
        std::vector<Job> one;
        one.push_back(std::move(j));
        RunSame(std::move(one), sample);
      }
      return;
    }
    // Input dims: [n] + sample.shape when batchable, else sample.shape.
    std::vector<std::int64_t> dims;
    if (batchable_) dims.push_back(static_cast<std::int64_t>(n));
    dims.insert(dims.end(), sample.shape.begin(), sample.shape.end());
    if (dims.size() != input_dims_.size()) {
      Status s = Status::InvalidArgument(Prefix() + "input rank " + std::to_string(sample.shape.size()) +
                                         " does not match model rank " + std::to_string(input_dims_.size()));
      for (Job& j : jobs) Complete(j, s, {});
      return;
    }
    for (std::size_t i = 1; i < dims.size(); ++i) {
      if (input_dims_[i] > 0 && input_dims_[i] != dims[i]) {
        Status s = Status::InvalidArgument(Prefix() + "input dim[" + std::to_string(i) + "]=" + std::to_string(dims[i]) +
                                           " does not match model " + std::to_string(input_dims_[i]));
        for (Job& j : jobs) Complete(j, s, {});
        return;
      }
    }
    const std::size_t sample_bytes = sample.ByteSize();
    std::vector<std::uint8_t> staging(sample_bytes * n);
    for (std::size_t i = 0; i < n; ++i) {
      const Packet& in = *FirstData(jobs[i]);
      std::memcpy(staging.data() + i * sample_bytes, in.payload->data, sample_bytes);
    }
    try {
      Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      Ort::Value input = Ort::Value::CreateTensor(mem, staging.data(), staging.size(), dims.data(), dims.size(),
                                                  ToOrt(input_dtype_));
      const char* in_names[] = {input_name_.c_str()};
      const char* out_names[] = {output_name_.c_str()};
      std::vector<Ort::Value> outs = session_->Run(Ort::RunOptions{nullptr}, in_names, &input, 1, out_names, 1);
      const auto info = outs[0].GetTensorTypeAndShapeInfo();
      std::vector<std::int64_t> odims = info.GetShape();
      const auto* obytes = static_cast<const std::uint8_t*>(outs[0].GetTensorRawData());
      // Per-member slice: leading dim is the batch when batchable.
      TensorFormat of;
      of.dtype = output_dtype_;
      std::size_t slice_elems = 1;
      if (batchable_ && !odims.empty() && odims[0] == static_cast<std::int64_t>(n)) {
        of.shape.assign(odims.begin() + 1, odims.end());
      } else {
        of.shape = odims;
      }
      if (of.shape.empty()) of.shape.push_back(1);
      for (const std::int64_t d : of.shape) slice_elems *= static_cast<std::size_t>(d);
      const std::size_t slice_bytes = slice_elems * SizeOf(output_dtype_);
      for (std::size_t i = 0; i < n; ++i) {
        const Packet& in = *FirstData(jobs[i]);
        BufferRef buf = pool_->Allocate(slice_bytes);
        std::memcpy(buf->data, obytes + i * slice_bytes, slice_bytes);
        buf->size = slice_bytes;
        Packet out = MakeTensorPacket(std::move(buf), of, in.header.seq, in.header.pts_ns);
        out.header.dts_ns = in.header.dts_ns;
        std::vector<CompletionOutput> outputs;
        outputs.push_back({"out", std::move(out)});
        Complete(jobs[i], Status::Ok(), std::move(outputs));
      }
    } catch (const Ort::Exception& e) {
      Status s = Status::Internal(Prefix() + "onnxruntime run: " + e.what());
      for (Job& j : jobs) Complete(j, s, {});
    }
  }

  static const Packet* FirstData(const Job& j) {
    for (const PacketRef& p : j.inputs) {
      if (!p->is_event() && !p->is_eos()) return p.get();
    }
    return nullptr;
  }

  void Complete(Job& j, Status status, std::vector<CompletionOutput> outputs) {
    CompletionEvent ev;
    ev.request_id = j.req.request_id;
    ev.session_id = j.req.session_id;
    ev.topology_version = j.req.topology_version;
    ev.parameter_version = j.req.parameter_version;
    ev.packet_seq = j.req.packet_seq;
    ev.status = std::move(status);
    ev.outputs = std::move(outputs);
    (void)j.sink->Push(std::move(ev));  // RESOURCE_EXHAUSTED is counted by the runtime
    j.inputs.clear();
  }

  JsonValue options_;
  std::string external_id_;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
  std::string input_name_, output_name_;
  DType input_dtype_ = DType::kFloat32, output_dtype_ = DType::kFloat32;
  std::vector<std::int64_t> input_dims_;
  bool batchable_ = false;
  std::chrono::milliseconds simulate_delay_{0};
  std::shared_ptr<HostBufferPool> pool_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  bool stop_ = false;
  bool fast_ = false;
  std::vector<std::thread> workers_;
};

}  // namespace

std::unique_ptr<Operator> MakeOnnxInfer(const OperatorCreateArgs& args) {
  return std::make_unique<OnnxInfer>(args.options);
}

bool OnnxRuntimeAvailable() noexcept { return true; }
std::string_view OnnxRuntimeVersion() noexcept {
  static const std::string kVersion = OrtGetApiBase()->GetVersionString();
  return kVersion;
}

#else  // !GE_HAVE_ONNXRUNTIME

namespace {
class OnnxUnavailable final : public Operator {
 public:
  Status Open(const OpenRequest& r) override {
    return Status::ResourceExhausted("node '" + std::string(r.external_id) +
                                     "': ge_infer was built without ONNX Runtime");
  }
  Result<ProcessResult> Process(const ProcessRequest&) override { return ProcessResult::kContinue; }
  Status Close(const CloseRequest&) override { return Status::Ok(); }
};
}  // namespace

std::unique_ptr<Operator> MakeOnnxInfer(const OperatorCreateArgs&) { return std::make_unique<OnnxUnavailable>(); }
bool OnnxRuntimeAvailable() noexcept { return false; }
std::string_view OnnxRuntimeVersion() noexcept { return ""; }

#endif

std::vector<ResourceAmount> EstimateOnnxInfer(const CapabilityDescriptor& cap, const JsonValue& options) {
  std::vector<ResourceAmount> out = EstimateNodeResources(cap);
  const std::int64_t intra = std::max<std::int64_t>(1, options.GetInteger("intra_threads").value_or(1));
  const std::int64_t workers = std::max<std::int64_t>(1, options.GetInteger("workers").value_or(1));
  for (ResourceAmount& a : out) {
    if (a.kind == ResourceKind::kCpuThreads) a.amount = static_cast<std::uint64_t>(intra * workers);
  }
  if (const auto model = options.GetString("model"); model && !model->empty()) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(*model, ec);
    if (!ec && size > 0) {
      std::erase_if(out, [](const ResourceAmount& a) { return a.kind == ResourceKind::kHostMemory; });
      out.push_back({ResourceKind::kHostMemory, -1, static_cast<std::uint64_t>(size) * 2});
    }
  }
  return out;
}

void RegisterOnnxInfer(BuiltinOperatorFactory& factory) {
  factory.Register(OnnxInferCapability(), &MakeOnnxInfer, &EstimateOnnxInfer);
}

}  // namespace ge::infer
