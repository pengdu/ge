#ifndef GE_TESTS_UNIT_TEST_OPERATORS_H_
#define GE_TESTS_UNIT_TEST_OPERATORS_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
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
                                PortCardinality card = PortCardinality::kSingle,
                                bool required = true) {
  PortCapability p;
  p.name = name;
  p.direction = dir;
  p.type_tag = "VideoFrame";
  p.required = required;
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
//
// Backpressure shape: one shared cursor plus a per-port done flag for the
// current seq. A port that already delivered the current seq is skipped on
// retries; a blocked port re-attempts the same seq (never yet delivered, so
// no duplicate) on the next call. The cursor only advances when every port
// has delivered, keeping the two ports in lockstep and each port's emitted
// seqs strictly increasing with no duplicates -- a re-emitted seq would
// otherwise look like two different keyframes to the in-band format-event
// mirror. Chosen over buffering pending Packets because the packet is
// deterministic from the cursor, so rebuilding it on retry is free and the
// state cannot disagree with itself.
class VideoCountSource final : public Operator {
 public:
  explicit VideoCountSource(std::int64_t count) : count_(count) {}
  Status Open(const OpenRequest&) override { return Status::Ok(); }
  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ProcessResult::kContinue;
    if (next_ >= count_) return ProcessResult::kExhausted;
    // Try every port that has not delivered the current seq yet. A blocking
    // port simply stays undelivered for the next call; it must not stop the
    // others from making progress.
    if (!out_done_) {
      const Status s = req.sink->Emit("out", MakePacket(GE_PACKET_FLAG_KEYFRAME, "VideoFrame"));
      if (s.code() == GE_STATUS_WOULD_BLOCK) {
        // leave out_done_ false: retry this same (never-delivered) seq later
      } else if (!s.ok()) {
        return s;
      } else {
        out_done_ = true;
      }
    }
    if (!bytes_done_) {
      const Status s = req.sink->Emit("bytes", MakePacket(0, "Bytes"));
      if (s.code() == GE_STATUS_WOULD_BLOCK) {
        // leave bytes_done_ false: retry this same seq later
      } else if (!s.ok()) {
        return s;
      } else {
        bytes_done_ = true;
      }
    }
    // The cursor only steps once every port has delivered the current seq.
    if (out_done_ && bytes_done_) {
      ++next_;
      out_done_ = false;
      bytes_done_ = false;
    }
    return ProcessResult::kContinue;
  }
  Status Close(const CloseRequest&) override { return Status::Ok(); }

 private:
  Packet MakePacket(std::uint32_t flags, const char* tag) const {
    Packet p;
    p.header.seq = static_cast<PacketSeq>(next_ + 1);
    p.header.pts_ns = next_ + 1;
    p.header.flags = flags;
    p.header.type_tag = TypeTagRegistry::Global().Intern(tag);
    return p;
  }

  std::int64_t count_;
  std::int64_t next_ = 0;
  bool out_done_ = false;
  bool bytes_done_ = false;
};

// Publishes "media_format_changed" and then emits one packet in the same
// Process call, so the scheduler sees the publication and the emit that may
// bind to it within a single invocation.
//
// |detail_seq| is the seq the detail names as its keyframe; it is a separate
// parameter from |key_seq_| on purpose, so a test can publish a binding that
// does not match what the node emits. |keyframe| controls the emitted
// packet's GE_PACKET_FLAG_KEYFRAME: with it false, a packet whose seq matches
// |detail_seq| still must not consume the candidate, because the binding is
// keyframe-specific, not seq-specific.
//
// Each entry of |hooks| runs once, in order, one immediately before the
// publish and one immediately before the emit, so a test can change engine
// state in between -- cancelling the node there makes the *event* push fail
// while the publication is already staged.
//
// |bytes_seq|, when non-zero, emits an opaque "Bytes" packet on a second
// output port "bytes" first, and |out2_seq| a keyframe on the video sibling
// "out2". Both are emitted *before* the keyframe on "out", which carries
// |key_seq|. The order matters: the candidate names |detail_seq|, emitted on
// "out" last, so an earlier emit on another port must leave it staged. Keying
// on seq alone would consume it on whichever port matched first.
//
// |repeat| publishes the same detail more than once before the emit
// (Review Focus #4: a duplicate publication must still mirror exactly one
// event Packet per edge). |pre_seqs| emits plain (non-keyframe) data packets
// on "out" before the first hook runs, so a fan-out test can wedge a
// capacity-1 block edge before the publish: the hook runs after them, so
// waiting on the consumer there proves the edge is full for the event and the
// keyframe that follow.
class FormatAnnouncer final : public Operator {
 public:
  explicit FormatAnnouncer(PacketSeq key_seq, PacketSeq detail_seq, bool keyframe = true,
                           std::vector<std::function<void()>> hooks = {}, PacketSeq out2_seq = 0,
                           PacketSeq bytes_seq = 0, int repeat = 1,
                           std::vector<PacketSeq> pre_seqs = {})
      : key_seq_(key_seq),
        detail_seq_(detail_seq),
        keyframe_(keyframe),
        hooks_(std::move(hooks)),
        out2_seq_(out2_seq),
        bytes_seq_(bytes_seq),
        repeat_(repeat),
        pre_seqs_(std::move(pre_seqs)) {}
  Status Open(const OpenRequest&) override { return Status::Ok(); }
  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ProcessResult::kContinue;
    if (done_) return ProcessResult::kExhausted;
    done_ = true;
    for (const PacketSeq s : pre_seqs_) {
      Packet p;
      p.header.seq = s;
      p.header.pts_ns = static_cast<std::int64_t>(s);
      p.header.flags = 0;  // plain data: must not touch any staged candidate
      p.header.type_tag = TypeTagRegistry::Global().Intern("VideoFrame");
      if (const Status st = req.sink->Emit("out", std::move(p)); !st.ok()) return st;
    }
    Hook();
    if (req.events != nullptr) {
      for (int i = 0; i < repeat_; ++i) {
        JsonObject d;
        d.emplace("first_key_seq", JsonValue(static_cast<std::uint64_t>(detail_seq_)));
        d.emplace("pixel_format", JsonValue("NV12"));
        req.events->Publish("media_format_changed", Severity::kInfo, JsonValue(std::move(d)));
      }
    }
    if (bytes_seq_ != 0) {
      if (const Status s = req.sink->Emit("bytes", MakeKeyframe(bytes_seq_, "Bytes")); !s.ok()) {
        return s;
      }
    }
    if (out2_seq_ != 0) {
      if (const Status s = req.sink->Emit("out2", MakeKeyframe(out2_seq_)); !s.ok()) return s;
    }
    Hook();
    // Not `return req.sink->Emit(...)`: Emit returns Status, and an OK Status
    // fed to Result<ProcessResult> becomes INTERNAL ("Result constructed from
    // OK status without value"), which failed the node after a successful
    // call and silently ate the EOS -- any test that waits for the graph to
    // close would hang on that.
    if (const Status s = req.sink->Emit("out", MakeKeyframe(key_seq_)); !s.ok()) return s;
    return ProcessResult::kContinue;  // next call reports kExhausted
  }
  Status Close(const CloseRequest&) override { return Status::Ok(); }
 private:
  Packet MakeKeyframe(PacketSeq seq, const char* tag = "VideoFrame") const {
    Packet p;
    p.header.seq = seq;
    p.header.pts_ns = 1000;
    p.header.flags = keyframe_ ? GE_PACKET_FLAG_KEYFRAME : 0u;
    p.header.type_tag = TypeTagRegistry::Global().Intern(tag);
    return p;
  }
  void Hook() {
    if (next_hook_ < hooks_.size()) hooks_[next_hook_++]();
  }

  PacketSeq key_seq_;
  PacketSeq detail_seq_;
  bool keyframe_;
  std::vector<std::function<void()>> hooks_;
  std::size_t next_hook_ = 0;
  PacketSeq out2_seq_ = 0;
  PacketSeq bytes_seq_ = 0;
  int repeat_ = 1;
  std::vector<PacketSeq> pre_seqs_;
  bool done_ = false;
};

// Records events and data separately, in arrival order, with the port each
// arrived on. Unlike Collector it keeps event packets in the same list, so a
// test can assert the event/record relative order that the in-band mirror
// promises. |calls| counts non-flush Process invocations, so a sync test can
// assert how many batches were produced, not just which packets arrived.
//
// Constructed gated, it blocks inside Process (after counting |entered|,
// before recording) until Release() -- the deterministic way to keep a
// capacity-1 block edge full while a producer emits behind it. Same
// contract as Gate: Release() opens it permanently.
class EventAwareCollector final : public Operator {
 public:
  explicit EventAwareCollector(bool gated = false) : gated_(gated) {}
  struct Entry {
    PacketSeq seq;
    bool event;
    std::string port;
    TopologyVersion topology_version;
    ParameterVersion parameter_version;
  };
  Status Open(const OpenRequest&) override { return Status::Ok(); }
  Result<ProcessResult> Process(const ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ProcessResult::kContinue;
    if (gated_) {
      std::unique_lock lock(gate_mutex_);
      ++entered;
      gate_cv_.wait(lock, [this] { return open_; });
    }
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < req.inputs.size(); ++i) {
      entries.push_back({req.inputs[i]->header.seq, req.inputs[i]->is_event(),
                         std::string(req.input_ports[i]),
                         req.inputs[i]->header.topology_version,
                         req.inputs[i]->header.parameter_version});
    }
    ++calls;
    return ProcessResult::kContinue;
  }
  Status Close(const CloseRequest&) override { return Status::Ok(); }
  std::vector<Entry> Entries() const {
    std::lock_guard lock(mutex_);
    return entries;
  }
  void Release() {
    {
      std::lock_guard lock(gate_mutex_);
      open_ = true;
    }
    gate_cv_.notify_all();
  }
  std::atomic<int> entered{0};
  std::atomic<int> calls{0};

 private:
  bool gated_ = false;
  std::mutex gate_mutex_;
  std::condition_variable gate_cv_;
  bool open_ = false;
  mutable std::mutex mutex_;
  std::vector<Entry> entries;
};

// Registers the in-band format-event pair: "Announce@1.0.0" (a video source
// that publishes "media_format_changed" bound to |detail_seq| and then emits
// its keyframe on "out") and "Collect@1.0.0" (an EventAwareCollector on "in").
// |key_seq| is what the node actually emits, so the two differ whenever a
// test wants a publication that cannot bind. |two_video_outputs| gives the
// announcer a second video port "out2", and |out2_seq| the keyframe it emits
// on it (the mirror is bound per output port, not per seq, and this is what
// lets a test prove it). |hooks| reach FormatAnnouncer unchanged.
inline void RegisterFormatAnnouncerFixture(BuiltinOperatorFactory& factory,
                                           std::vector<std::shared_ptr<Operator>>& keep,
                                           PacketSeq key_seq, PacketSeq detail_seq,
                                           EventAwareCollector** collector,
                                           bool keyframe = true,
                                           bool two_video_outputs = false,
                                           PacketSeq out2_seq = 0,
                                           std::vector<std::function<void()>> hooks = {},
                                           bool opaque_output = false,
                                           PacketSeq bytes_seq = 0,
                                           int repeat = 1,
                                           std::vector<PacketSeq> pre_seqs = {}) {
  std::vector<PortCapability> outs{
      VideoPort("out", PortDirection::kOutput, {"NV12"}, PortCardinality::kMulti)};
  if (opaque_output) {
    outs.push_back(BytesPort("bytes", PortDirection::kOutput, false, PortCardinality::kMulti));
  }
  if (two_video_outputs) {
    outs.push_back(VideoPort("out2", PortDirection::kOutput, {"NV12"}, PortCardinality::kMulti));
  }
  factory.Register(Desc("Announce@1.0.0", {}, std::move(outs), true, 1),
                   [&keep, key_seq, detail_seq, keyframe, out2_seq, hooks, bytes_seq, repeat,
                    pre_seqs](const OperatorCreateArgs&) {
                     return Keep(keep, std::make_shared<FormatAnnouncer>(
                                           key_seq, detail_seq, keyframe, hooks, out2_seq,
                                           bytes_seq, repeat, pre_seqs));
                   });
  // "in" is required (the video leg of every fixture graph); "in2" is optional
  // so a two-video test can wire the sibling, and the opaque leg is optional.
  factory.Register(Desc("Collect@1.0.0",
                        {VideoPort("in", PortDirection::kInput, {"NV12"}),
                         VideoPort("in2", PortDirection::kInput, {"NV12"}, PortCardinality::kMulti,
                                   false),
                         BytesPort("bytes", PortDirection::kInput, false)},
                        {}),
                   [&keep, collector](const OperatorCreateArgs&) {
                     auto c = std::make_shared<EventAwareCollector>();
                     *collector = c.get();
                     return Keep(keep, std::move(c));
                   });
}

// Registers a video-formatted producer/consumer pair for EVT-4 tests. The
// ports carry VideoConstraints so the negotiated contract has a video
// format and IsVideoRoute() is true, with no media build required.
// |detail_seq| differs from the announced keyframe seq in the negative
// cases; |repeat| publishes the same detail more than once. Thin wrapper:
// RegisterFormatAnnouncerFixture stays the single registration path, this
// is just the short spelling for the common positive-shape tests.
inline void RegisterFormatEventPair(BuiltinOperatorFactory& factory,
                                    std::vector<std::shared_ptr<Operator>>& keep,
                                    EventAwareCollector** sink, PacketSeq key_seq,
                                    PacketSeq detail_seq, int repeat = 1) {
  RegisterFormatAnnouncerFixture(factory, keep, key_seq, detail_seq, sink,
                                 /*keyframe=*/true, /*two_video_outputs=*/false,
                                 /*out2_seq=*/0, /*hooks=*/{}, /*opaque_output=*/false,
                                 /*bytes_seq=*/0, repeat);
}

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
