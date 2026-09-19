#ifndef GE_TESTS_UNIT_TEST_OPERATORS_H_
#define GE_TESTS_UNIT_TEST_OPERATORS_H_

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ge/cpp/operator.h>

namespace ge::test {

inline PortCapability BytesPort(const char* name, PortDirection dir, bool required = true,
                                PortCardinality card = PortCardinality::kSingle) {
  PortCapability p;
  p.name = name;
  p.direction = dir;
  p.type_tag = "Bytes";
  p.required = required;
  p.cardinality = card;
  return p;
}

inline CapabilityDescriptor Desc(const char* key, std::vector<PortCapability> ins,
                                 std::vector<PortCapability> outs, bool stateful = false,
                                 std::uint32_t max_par = 4) {
  CapabilityDescriptor d;
  d.op = *OperatorKey::Parse(key);
  d.inputs = std::move(ins);
  d.outputs = std::move(outs);
  d.execution.stateful = stateful;
  d.execution.max_parallelism = max_par;
  d.parameters.hot_updatable = {"gain"};
  return d;
}

// Emits |count| packets on "out" then reports exhausted.
class CountingSource final : public Operator {
 public:
  explicit CountingSource(std::int64_t count, std::shared_ptr<HostBufferPool> pool,
                          std::int64_t pts_step = 1)
      : count_(count), pool_(std::move(pool)), pts_step_(pts_step) {}
  Status Open(const OpenRequest&) override { return Status::Ok(); }
  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ProcessResult::kContinue;
    if (next_ >= count_) return ProcessResult::kExhausted;
    Packet p;
    p.header.seq = static_cast<PacketSeq>(++next_);
    p.header.pts_ns = next_ * pts_step_;
    p.header.type_tag = TypeTagRegistry::Global().Intern("Bytes");
    p.payload = pool_->Allocate(8);
    std::memcpy(p.payload->data, &p.header.seq, sizeof(p.header.seq));
    Status s = req.sink->Emit("out", std::move(p));
    if (s.code() == GE_STATUS_WOULD_BLOCK) {
      --next_;  // retry the same packet later
      ++blocked;
    } else if (!s.ok()) {
      return s;
    }
    return ProcessResult::kContinue;
  }
  Status Close(const CloseRequest&) override {
    closed = true;
    return Status::Ok();
  }
  std::atomic<bool> closed{false};
  std::atomic<int> blocked{0};

 private:
  std::int64_t count_;
  std::shared_ptr<HostBufferPool> pool_;
  std::int64_t pts_step_;
  std::int64_t next_ = 0;
};

// Forwards every input packet to "out"; optional delay; records order.
class PassThrough final : public Operator {
 public:
  explicit PassThrough(std::chrono::microseconds delay = {}) : delay_(delay) {}
  Status Open(const OpenRequest&) override {
    opened = true;
    return Status::Ok();
  }
  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) {
      flushed = true;
      return ProcessResult::kContinue;
    }
    if (delay_.count() > 0) std::this_thread::sleep_for(delay_);
    for (const PacketRef& in : req.inputs) {
      if (in->is_event()) continue;
      Packet out = *in;  // shares payload
      last_param_version = req.parameter_version;
      std::int64_t gain = 0;
      if (req.parameters != nullptr) {
        if (const auto g = req.parameters->GetInteger("gain")) gain = *g;
      }
      gain_seen = gain;
      {
        std::lock_guard lock(mutex_);
        records_.push_back({in->header.seq, req.parameter_version, gain, req.topology_version});
      }
      Status s = req.sink->Emit("out", std::move(out));
      if (!s.ok() && s.code() != GE_STATUS_WOULD_BLOCK) return s;
    }
    return ProcessResult::kContinue;
  }
  Status Close(const CloseRequest& r) override {
    closed = true;
    fast_close = r.fast_shutdown;
    return Status::Ok();
  }
  struct Record {
    PacketSeq seq;
    ParameterVersion parameter_version;
    std::int64_t gain;
    TopologyVersion topology_version;
  };
  std::vector<Record> Records() const {
    std::lock_guard lock(mutex_);
    return records_;
  }
  std::atomic<bool> opened{false}, flushed{false}, closed{false}, fast_close{false};
  std::atomic<ParameterVersion> last_param_version{0};
  std::atomic<std::int64_t> gain_seen{0};

 private:
  std::chrono::microseconds delay_;
  mutable std::mutex mutex_;
  std::vector<Record> records_;
};

// Collects packets; thread-safe.
class Collector final : public Operator {
 public:
  Status Open(const OpenRequest&) override { return Status::Ok(); }
  Result<ProcessResult> Process(const ProcessRequest& req) override {
    std::lock_guard lock(mutex_);
    if (req.flags & GE_PROCESS_FLAG_FLUSH) {
      flushed = true;
      return ProcessResult::kContinue;
    }
    for (std::size_t i = 0; i < req.inputs.size(); ++i) {
      seqs.push_back(req.inputs[i]->header.seq);
      ports.emplace_back(req.input_ports[i]);
      Packet copy = *req.inputs[i];
      copy.payload.Reset();  // headers/meta only: keep pool accounting exact
      packets.push_back(std::move(copy));
      if (req.inputs[i]->payload) payload_refs.push_back(req.inputs[i]->payload.use_count());
    }
    ++calls;
    return ProcessResult::kContinue;
  }
  Status Close(const CloseRequest& r) override {
    std::lock_guard lock(mutex_);
    closed = true;
    fast_close = r.fast_shutdown;
    return Status::Ok();
  }
  std::vector<PacketSeq> Seqs() const {
    std::lock_guard lock(mutex_);
    return seqs;
  }
  std::vector<std::string> Ports() const {
    std::lock_guard lock(mutex_);
    return ports;
  }
  std::vector<Packet> Packets() const {
    std::lock_guard lock(mutex_);
    return packets;
  }
  std::atomic<bool> flushed{false}, closed{false}, fast_close{false};
  std::atomic<int> calls{0};
  std::vector<std::uint32_t> payload_refs;

 private:
  mutable std::mutex mutex_;
  std::vector<PacketSeq> seqs;
  std::vector<std::string> ports;
  std::vector<Packet> packets;
};

// Fails on the N-th packet.
class Faulty final : public Operator {
 public:
  explicit Faulty(int fail_at) : fail_at_(fail_at) {}
  Status Open(const OpenRequest&) override { return Status::Ok(); }
  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ProcessResult::kContinue;
    if (++seen_ == fail_at_) return Status::Internal("boom");
    for (const PacketRef& in : req.inputs) (void)req.sink->Emit("out", *in);
    return ProcessResult::kContinue;
  }
  Status Close(const CloseRequest&) override { return Status::Ok(); }

 private:
  int fail_at_;
  int seen_ = 0;
};

class FailsToOpen final : public Operator {
 public:
  Status Open(const OpenRequest&) override { return Status::ResourceExhausted("no gpu"); }
  Result<ProcessResult> Process(const ProcessRequest&) override { return ProcessResult::kContinue; }
  Status Close(const CloseRequest&) override { return Status::Ok(); }
};

// Test fixtures keep raw pointers to operators after the engine released
// the NodeRuntime (closed nodes drop their operator with the topology).
// Shared<T> owns the real instance; the engine gets a forwarding shell.
template <typename T>
class Shared final : public Operator {
 public:
  explicit Shared(std::shared_ptr<T> impl) : impl_(std::move(impl)) {}
  Status Open(const OpenRequest& r) override { return impl_->Open(r); }
  Result<ProcessResult> Process(const ProcessRequest& r) override { return impl_->Process(r); }
  Status Submit(const SubmitRequest& r) override { return impl_->Submit(r); }
  Status Close(const CloseRequest& r) override { return impl_->Close(r); }

 private:
  std::shared_ptr<T> impl_;
};

// Registers |impl| in |keep| and returns the engine-facing shell.
template <typename T>
std::unique_ptr<Operator> Keep(std::vector<std::shared_ptr<Operator>>& keep, std::shared_ptr<T> impl) {
  keep.push_back(impl);
  return std::make_unique<Shared<T>>(std::move(impl));
}

}  // namespace ge::test

#endif
