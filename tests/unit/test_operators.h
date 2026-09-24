#ifndef GE_TESTS_UNIT_TEST_OPERATORS_H_
#define GE_TESTS_UNIT_TEST_OPERATORS_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ge/cpp/operator.h>
#include <ge/cpp/runtime_topology.h>

namespace ge::test {

// True when no packet is in flight anywhere in |topo|: every node is
// quiescent (no invoke task running or scheduled) and every edge is empty.
// Scanned in topological order so a packet cannot slip behind the scan: it
// can only outrun it, in which case it already reached its sink. Tests use
// this instead of "the sink count stopped changing for N ms", which is a
// timing guess that fails under CPU starvation (stress runs).
inline bool DrainedOnce(const RuntimeTopology& topo) {
  for (const std::string& id : topo.topological_order()) {
    const NodeRuntime* n = topo.FindNode(id);
    if (n == nullptr) continue;
    if (!n->quiescent()) return false;
    for (const EdgeChannelRef& e : topo.edges()) {
      const auto producer = e->producer();
      if (producer.get() == n && !e->empty()) return false;
    }
  }
  return true;
}

// Waits until DrainedOnce() holds for |confirmations| consecutive scans.
inline bool WaitDrained(const RuntimeTopology& topo, std::chrono::milliseconds timeout,
                        int confirmations = 3) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int clear = 0;
  while (clear < confirmations) {
    if (DrainedOnce(topo)) {
      ++clear;
    } else {
      clear = 0;
      if (std::chrono::steady_clock::now() > deadline) return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

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

// A video port: carries VideoConstraints, which is what IsVideoRoute() reads.
inline PortCapability VideoPort(const char* name, PortDirection dir, std::vector<std::string> pf,
                                PortCardinality card = PortCardinality::kSingle) {
  PortCapability p;
  p.name = name;
  p.direction = dir;
  p.type_tag = "VideoFrame";
  p.cardinality = card;
  p.video = VideoConstraints{};
  p.video->pixel_formats = std::move(pf);
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

// A misbehaving operator that ignores the engine's stop request: holds every
// Process call until |release| is set on the shared control, then reports
// exhausted. Instances share the control so the test can release them all.
class StuckSource final : public Operator {
 public:
  struct Control {
    std::atomic<bool> release{false};
    std::atomic<int> stuck_calls{0};
  };
  explicit StuckSource(std::shared_ptr<Control> control) : control_(std::move(control)) {}
  Status Open(const OpenRequest&) override { return Status::Ok(); }
  Result<ProcessResult> Process(const ProcessRequest&) override {
    control_->stuck_calls.fetch_add(1, std::memory_order_relaxed);
    while (!control_->release.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ProcessResult::kExhausted;
  }
  Status Close(const CloseRequest&) override { return Status::Ok(); }

 private:
  std::shared_ptr<Control> control_;
};

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

// Forwards its input but blocks inside Process until Release() is called.
// Deterministic replacement for "slow operator + short drain timeout"
// tests: a sleep-based slow node is a timing guess that fails when the
// stress runner starves the executor. The engine never interrupts a
// running Process call, so the test must Release() before expecting the
// node to close.
class Gate final : public Operator {
 public:
  Status Open(const OpenRequest&) override { return Status::Ok(); }
  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ProcessResult::kContinue;
    {
      std::unique_lock lock(mutex_);
      ++entered;
      cv_.wait(lock, [this] { return open_; });
    }
    for (const PacketRef& in : req.inputs) {
      if (in->is_event()) continue;
      Packet out = *in;
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
  void Release() {
    {
      std::lock_guard lock(mutex_);
      open_ = true;
    }
    cv_.notify_all();
  }
  std::atomic<int> entered{0};
  std::atomic<bool> closed{false}, fast_close{false};

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
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

// Emits |count| VideoFrame packets on "out" and |count| opaque Bytes packets
// on "bytes", then reports exhausted. Both ports carry the same seq so a
// consumer can pair them; "out" is the video path IsVideoRoute() must see.
class VideoCountSource final : public Operator {
 public:
  explicit VideoCountSource(std::int64_t count) : count_(count) {}
  Status Open(const OpenRequest&) override { return Status::Ok(); }
  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ProcessResult::kContinue;
    if (next_ >= count_) return ProcessResult::kExhausted;
    const PacketSeq seq = static_cast<PacketSeq>(next_ + 1);
    Packet v;
    v.header.seq = seq;
    v.header.pts_ns = next_ + 1;
    v.header.flags = GE_PACKET_FLAG_KEYFRAME;
    v.header.type_tag = TypeTagRegistry::Global().Intern("VideoFrame");
    const Status vs = req.sink->Emit("out", std::move(v));
    if (!vs.ok() && vs.code() != GE_STATUS_WOULD_BLOCK) return vs;
    Packet b;
    b.header.seq = seq;
    b.header.pts_ns = next_ + 1;
    b.header.type_tag = TypeTagRegistry::Global().Intern("Bytes");
    const Status bs = req.sink->Emit("bytes", std::move(b));
    if (!bs.ok() && bs.code() != GE_STATUS_WOULD_BLOCK) return bs;
    if (vs.code() == GE_STATUS_WOULD_BLOCK || bs.code() == GE_STATUS_WOULD_BLOCK) {
      return ProcessResult::kContinue;  // retry this seq later
    }
    ++next_;
    return ProcessResult::kContinue;
  }
  Status Close(const CloseRequest&) override { return Status::Ok(); }

 private:
  std::int64_t count_;
  std::int64_t next_ = 0;
};

// A video source with one video output ("out", NV12+P010, multi) and one
// opaque output ("bytes", a plain Bytes port, so IsVideoRoute() is false for
// it), plus a video sink ("in", NV12). Shared by the tests that need a graph
// where exactly one edge is a video route; Task 4 reuses it rather than
// registering a second copy.
inline void RegisterVideoFixture(BuiltinOperatorFactory& factory,
                                 std::vector<std::shared_ptr<Operator>>& keep) {
  factory.Register(Desc("VSrc@1.0.0", {},
                        {VideoPort("out", PortDirection::kOutput, {"NV12", "P010"},
                                   PortCardinality::kMulti),
                         BytesPort("bytes", PortDirection::kOutput, false)},
                        true, 1),
                   [&keep](const OperatorCreateArgs& a) {
                     auto s = std::make_shared<VideoCountSource>(a.options.GetInteger("count").value_or(1));
                     return Keep(keep, std::move(s));
                   });
  factory.Register(Desc("VSink@1.0.0", {VideoPort("in", PortDirection::kInput, {"NV12"})}, {}),
                   [&keep](const OperatorCreateArgs&) {
                     auto c = std::make_shared<Collector>();
                     return Keep(keep, std::move(c));
                   });
}

}  // namespace ge::test

#endif
