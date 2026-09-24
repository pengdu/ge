#ifndef GE_CPP_INPUT_BINDING_H_
#define GE_CPP_INPUT_BINDING_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ge/cpp/edge_channel.h>
#include <ge/cpp/packet.h>

namespace ge {

struct InputPortBinding {
  std::string port;
  EdgeChannelRef edge;  // exactly one edge per input port
  bool required = true;
  // Retired edges that fed this port in earlier topology versions and still
  // marker before |edge| is touched, so old packets keep the old path.
  std::vector<EdgeChannelRef> predecessors;
};

struct InputBatch {
  std::vector<std::string> ports;
  std::vector<PacketRef> packets;  // parallel to |ports|
  // Packets that skipped synchronisation and go straight to the request's
  // input list: control (EOS/events) never participates in alignment.
  std::vector<PacketRef> events;
  // Source port of each entry in |events| (parallel vector). Downstreams
  // need this to attribute a format event to one of several video inputs;
  // a blank name here would make a two-video-input consumer blind.
  std::vector<std::string> event_ports;
};

// ---------------------------------------------------------------------------
// In-band media format frontier (EVT-4).
//
// A producer publishes "media_format_changed" whose detail names the
// keyframe that starts the new configuration; the scheduler mirrors that
// publication into a GE_PACKET_FLAG_EVENT Packet placed immediately before
// that keyframe on every video output edge. See
// docs/superpowers/specs/2026-09-23-media-format-changed-design.md.
// ---------------------------------------------------------------------------

// The one event type mirrored into the data plane.
inline constexpr std::string_view kFormatEventType = "media_format_changed";
// The detail key that binds an event to its keyframe.
inline constexpr std::string_view kFormatEventSeqKey = "first_key_seq";

// True when an edge's negotiated contract carries a video format, i.e.
// the edge is a video data path. Takes the contract rather than the
// RouteEntry on purpose: RouteEntry lives in runtime_topology.h, which
// already includes this header (line 16), so naming it here would close
// an include cycle.
[[nodiscard]] bool IsVideoRoute(const ConnectionContract& contract);

// The keyframe seq this event binds to; nullopt when the detail carries no
// usable key (absent, non-numeric, non-positive). Such an event is
// side-band only.
[[nodiscard]] std::optional<PacketSeq> FormatEventMatchSeq(const JsonValue& detail);

// Builds the in-band event Packet for |keyframe|: identity fields copied,
// the event flag set, no payload.
[[nodiscard]] Packet MakeFormatEventPacket(const Packet& keyframe, std::string_view event_type,
                                           const JsonValue& detail);

struct AlignedOptions {
  std::int64_t window_ns = 40'000'000;  // ±40ms
  std::optional<std::string> reference_port;  // nullopt: earliest ready required port
};

// Mutation that changes the node's input edges calls Rebind() at publish.
class InputBinding final {
 public:
  InputBinding(std::vector<InputPortBinding> ports, SyncPolicy policy,
               AlignedOptions aligned = {});

  [[nodiscard]] SyncPolicy policy() const;
  // Snapshot (the live table may be rebound concurrently).
  [[nodiscard]] std::vector<InputPortBinding> ports() const;
  [[nodiscard]] std::optional<InputPortBinding> Find(std::string_view port) const;

  // previous edge (and its unfinished predecessors) become predecessors of
  // the new one. EOS state of unchanged ports is kept.
  void Rebind(std::vector<InputPortBinding> ports, SyncPolicy policy, AlignedOptions aligned);
  // Drops a predecessor that was fast-retired (queue cleared).
  void ForgetPredecessor(const EdgeChannel& edge);
  // Every edge currently feeding this binding (ports + predecessors).
  [[nodiscard]] std::vector<EdgeChannelRef> Edges() const;

  // Returns a batch when the sync condition holds. Consumes packets from
  // the edges. Returns nullopt when nothing can be delivered yet.
  // Acquisition is serialized: a node with parallelism > 1 runs Process
  // single consumer of its input edges.
  [[nodiscard]] std::optional<InputBatch> TryAcquire();

  [[nodiscard]] bool InputEnded() const noexcept;
  [[nodiscard]] std::set<std::string> eos_ports() const;  // observation (tests/snapshot)
  [[nodiscard]] std::uint64_t late_dropped() const noexcept { return late_dropped_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t stale_dropped() const noexcept { return stale_dropped_.load(std::memory_order_relaxed); }

  // Cheap check used by Scheduler before enqueueing the node.
  [[nodiscard]] bool MayBeReady() const noexcept;

 private:
  // Pops leading EOS/event packets of |b| into state; returns the first data
  // packet still queued (not popped) or nullptr. Drains predecessors first.
  PacketRef SkimHead(InputPortBinding& b, InputBatch* out);
  // Pops the head of the edge SkimHead last looked at; nullptr if the edge
  // was retired/evicted concurrently.
  static PacketRef Take(InputPortBinding& b);
  // The edge SkimHead/Take currently operate on for |b|.
  [[nodiscard]] static EdgeChannel* Active(const InputPortBinding& b) noexcept;
  std::optional<InputBatch> AcquireAny();
  std::optional<InputBatch> AcquireLatest();
  std::optional<InputBatch> AcquireAligned();
  [[nodiscard]] bool IsEos(std::string_view port) const noexcept { return eos_ports_.contains(std::string(port)); }
  [[nodiscard]] bool InputEndedLocked() const noexcept;

  std::vector<InputPortBinding> ports_;
  SyncPolicy policy_;
  AlignedOptions aligned_;
  mutable std::mutex mutex_;  // guards ports_/eos_ports_ and edge Peek/Pop sequences
  std::set<std::string> eos_ports_;
  std::atomic<std::uint64_t> late_dropped_{0};   // aligned: outside window
  std::atomic<std::uint64_t> stale_dropped_{0};  // latest: superseded packets
};

}  // namespace ge

#endif
