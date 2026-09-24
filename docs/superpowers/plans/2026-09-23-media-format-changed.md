# MediaFormatChanged In-Band Frontier Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `media_format_changed` reach downstream nodes as an in-band `GE_PACKET_FLAG_EVENT` Packet ordered immediately before the keyframe it describes, on every video output edge of the producing node, while keeping the existing side-band EventBus publication unchanged.

**Architecture:** The mirror decision lives in `Scheduler::Sink` (the only place holding topology, routes and the `Park` backpressure state), so both built-in operators (which publish through `req.events`) and so plugins (which publish through `host_api.event_publish`) share one hook. `Sink::Publish` records a candidate keyed by `detail.first_key_seq`; `Sink::Emit` sees a keyframe with a matching seq and pushes the event Packet through the *same* per-edge `HasParked`/`Park`/`PushOne` path, immediately before the keyframe. `InputBatch` gains a parallel `event_ports` vector so downstreams can tell which input port an event came from.

**Tech Stack:** C++20, CMake 3.25+, GoogleTest, the repo's own `RuntimeTopology` / `EdgeChannel` / `InputBinding` / `Scheduler` stack. No third-party additions.

**Spec:** `docs/superpowers/specs/2026-09-23-media-format-changed-design.md`

## Global Constraints

- Mirror qualification is hard-coded to the single event type `media_format_changed` (`ge::kEventMediaFormatChanged`). No other event type may be mirrored.
- The match key is `detail.first_key_seq`, a JSON integer, compared against `Packet::header.seq` (`ge::PacketSeq` = `std::uint64_t`). Absent / non-integer / `0` means "not mirrorable".
- `Packet::header.flags` on a mirrored event carries `GE_PACKET_FLAG_EVENT` (`0x4`) and nothing else — in particular not `GE_PACKET_FLAG_KEYFRAME` (would confuse `PacketRouter::Prepare`'s control-flow checks) and not `GE_PACKET_FLAG_DROPPED`.
- Only edges whose negotiated contract has a video format participate: `route.contract.video.has_value()`. Non-video edges receive the keyframe only.
- The side-band EventBus copy must be published **exactly once** per producer call, on every path.
- Event Packets must never be counted as media frames: `packets_out` / `packets_in` accounting for control packets stays as it is today (`src/runtime_topology.cpp:247`).
- Event Packets must not participate in multi-input alignment (they must not satisfy a required port, must not move the `kAligned` window, must not trigger `kLatest`). This already holds in `src/input_binding.cpp`; the plan must not regress it.
- New tests must run in the default configuration (`GE_ENABLE_MEDIA` may be `OFF` for the reviewer). Do not add tests that require FFmpeg, and do not modify `src/media/*` in any task of this plan.
- No public ABI change: `include/ge/c/*` headers and `ge_host_api`'s layout stay byte-identical. `GE_ABI_MAJOR` is not bumped.
- Every task ends green on: `cmake --build build -j` then `ctest --test-dir build --output-on-failure -LE "stress|soak"`.
- Commit style: the repo's `type(scope): summary` convention, imperative subject, body explaining the *why*.

## Review Focus

Failure modes this spec implies but whose tests are easy to skip. Each line names the input or condition and what a reasonable person expects; the owning task's steps add the test.

1. **Concurrent mutation while an event is pending** — a topology swap lands between the `Publish` that staged the candidate and the `Emit` that should mirror it. Expected: the event rides the Edge FIFO of whichever topology the emitter is bound to, never a retired edge, and the candidate does not leak into the next invocation.
2. **Two video output ports on one node, only one carrying the format change** — expected: the event is injected only on the port whose keyframe matched, not on the sibling video port.
3. **A plugin publishing `media_format_changed` outside a process call** (from `open`/`close`, or from an async worker thread after the call returned) — expected: side-band publication happens, no in-band Packet is created, nothing crashes, and the `HostApiAdapter` does not dereference a dead scope.
4. **Same `first_key_seq` published twice in one invocation** (retry loop or duplicated format report) — expected: exactly one event Packet per edge per keyframe, not two.
5. **Keyframe queued behind a parked packet on a `kBlock` edge** — expected: the event Packet still precedes the keyframe in that edge's FIFO after the park drains, and the edge's drop/overflow policy is applied identically to both.
6. **`Seq` that never becomes a keyframe** (the node publishes the event but the following keyframe is dropped, or the value is simply wrong) — expected: no in-band Packet, side-band still published, the un-mirrored counter advances exactly once, and the Sink does not hold the detail alive past the call.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `include/ge/cpp/input_binding.h` | modify | `InputBatch.event_ports`; declarations of the three format-event functions (public so tests can reach them) |
| `src/input_binding.cpp` | modify | record the source port when an event is skimmed out of an edge |
| `src/format_event.cpp` | create | `IsVideoRoute`, `FormatEventMatchSeq`, `MakeFormatEventPacket` |
| `src/scheduler_invoke.cpp` | modify | `MoveBatchInto` fills real event port names |
| `src/scheduler_internal.h` | modify | `Sink` stages candidates in `Publish` and injects them in `Emit` |
| `include/ge/cpp/node_runtime.h` | modify | `NodeMetrics::format_events_unmirrored` |
| `include/ge/cpp/plugin_operator.h` | modify | `InvokeScope` carries the call's `EventSink*` |
| `src/plugin_operator.cpp` | modify | `EventPublish` prefers the active scope's sink |
| `src/metrics_export.cpp` | modify | export `node_format_events_unmirrored_total` |
| `tests/unit/test_operators.h` | modify | shared format-event test doubles |
| `tests/unit/{input_binding,packet_edge,scheduler,metrics_export}_test.cpp` | modify | coverage |
| `tests/unit/plugin_event_test.cpp` | create | plugin-entry coverage |
| `tests/CMakeLists.txt`, `CMakeLists.txt` | modify | register the new test target and source file |
| `docs/{08,12,13}` | modify | sync the shipped contract |

Deliberately untouched: `src/media/*` (the existing `video_encode` producer needs no change — its `Publish` call becomes mirrorable the moment `Sink::Publish` stages it), and `include/ge/c/*` (no ABI change).

### Task 1: `InputBatch.event_ports` — event source-port attribution

**Files:**
- Modify: `include/ge/cpp/input_binding.h:26-33` (the `InputBatch` struct)
- Modify: `src/input_binding.cpp:142-181` (`SkimHead`, the `head->is_event()` branch at line 169)
- Modify: `src/scheduler_invoke.cpp:77-92` (`MoveBatchInto`)
- Test: `tests/unit/input_binding_test.cpp` (extend `EventPacketsBypassSync`), `tests/unit/scheduler_test.cpp` (new test)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `ge::InputBatch::event_ports` — a `std::vector<std::string>`, kept index-parallel to `InputBatch::events`. Later tasks and downstream operators rely on `req.input_ports[i]` being the true source port for every event in `req.inputs`.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/input_binding_test.cpp`, inside the existing `EventPacketsBypassSync` test, right after `ASSERT_EQ(batch->events.size(), 1U);` near the end (the batch that also has one data packet):

```cpp
  ASSERT_EQ(batch->event_ports.size(), batch->events.size());
  EXPECT_EQ(batch->event_ports[0], "video");
```

And a standalone case for the two-port form, appended before the closing `}  // namespace`:

```cpp
TEST(InputBindingTest, EventPacketCarriesItsSourcePort) {
  auto v = Edge("video");
  auto a = Edge("audio");
  ge::InputBinding in({{"video", v, true, {}}, {"audio", a, false, {}}}, ge::SyncPolicy::kAny);
  ASSERT_EQ(v->Push(Data(1, 0, GE_PACKET_FLAG_EVENT)), ge::PushOutcome::kAccepted);
  ASSERT_EQ(a->Push(Data(2, 0, GE_PACKET_FLAG_EVENT)), ge::PushOutcome::kAccepted);
  auto batch = in.TryAcquire();
  ASSERT_TRUE(batch.has_value());
  ASSERT_EQ(batch->events.size(), 2U);
  ASSERT_EQ(batch->event_ports.size(), 2U);
  // SkimHead walks ports_ in declaration order, so video precedes audio.
  EXPECT_EQ(batch->event_ports[0], "video");
  EXPECT_EQ(batch->event_ports[1], "audio");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build -j && ctest --test-dir build -R ge_input_binding_test --output-on-failure`
Expected: FAIL to compile — `ge::InputBatch` has no member named `event_ports`.

- [ ] **Step 3: Add the field**

In `include/ge/cpp/input_binding.h`, replace the `InputBatch` struct body:

```cpp
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
```

- [ ] **Step 4: Populate the field at the one place events are taken**

In `src/input_binding.cpp`, in `SkimHead`, the event branch currently reads:

```cpp
    if (head->is_event()) {
      if (PacketRef ev = Take(b)) out->events.push_back(std::move(ev));
      continue;
    }
```

Replace it with:

```cpp
    if (head->is_event()) {
      if (PacketRef ev = Take(b)) {
        out->events.push_back(std::move(ev));
        out->event_ports.push_back(b.port);
      }
      continue;
    }
```

- [ ] **Step 5: Carry the port names into the request**

In `src/scheduler_invoke.cpp`, replace `MoveBatchInto` (and the comment above it) with:

```cpp
// Moves an acquired batch into a request: control packets first (each with
// its true source port, so a downstream can attribute a format event to one
// of several inputs), then data, with a parallel port list (12 §4.5).
template <typename Ports>
void MoveBatchInto(InputBatch& batch, Ports& ports, std::vector<PacketRef>& inputs) {
  for (std::size_t i = 0; i < batch.events.size(); ++i) {
    ports.push_back(i < batch.event_ports.size() ? batch.event_ports[i] : std::string{});
    inputs.push_back(std::move(batch.events[i]));
  }
  for (std::size_t i = 0; i < batch.packets.size(); ++i) {
    ports.emplace_back(batch.ports[i]);
    inputs.push_back(std::move(batch.packets[i]));
  }
}
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build -j && ctest --test-dir build -R "ge_input_binding_test|ge_scheduler_test|ge_session_test" --output-on-failure`
Expected: PASS. All three suites exercise `MoveBatchInto`; a signature break would surface here.

- [ ] **Step 7: Add the scheduler-level attribution test**

Append to `tests/unit/scheduler_test.cpp`, next to the other fan-out tests. It asserts the observable contract that matters — a downstream sees the event's port name — through the real scheduler:

```cpp
TEST(SchedulerTest, EventPacketsReachCollectorWithTheirSourcePort) {
  Fixture f;
  ge::GraphBuilder b("event-ports");
  auto src = b.AddNode(Op("Src@1.0.0"), "src",
                       ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(std::int64_t{3})}}));
  auto sink = b.AddNode(Op("Sink@1.0.0"), "sink");
  b.Connect(src.port("out"), sink.port("in"), {.id = "e0"});
  auto topo = f.Build(*b.Build());
  ASSERT_TRUE(topo);
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  while (exec.RunPending()) {
  }
  const std::vector<std::string> ports = f.sinks[0]->Ports();
  const std::vector<ge::PacketSeq> seqs = f.sinks[0]->Seqs();
  ASSERT_EQ(ports.size(), seqs.size());
  for (const std::string& p : ports) EXPECT_EQ(p, "in");
}
```

This pins the field's plumbing end to end; Task 4 adds the case where the port names differ.

- [ ] **Step 8: Run it**

Run: `cmake --build build -j && ctest --test-dir build -R ge_scheduler_test --output-on-failure`
Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add include/ge/cpp/input_binding.h src/input_binding.cpp src/scheduler_invoke.cpp tests/unit/input_binding_test.cpp tests/unit/scheduler_test.cpp
git commit -m "feat(binding): carry the source port of in-band events

MoveBatchInto used to hand every control packet an empty port name, so a
consumer with two video inputs could not tell which stream a format
event described. InputBatch now keeps event_ports parallel to events."
```

---

### Task 2: Video-edge predicate and match-key parsing

**Files:**
- Modify: `include/ge/cpp/input_binding.h` (add the three free function declarations next to `InputBatch`)
- Create: `src/format_event.cpp`
- Modify: `src/scheduler_internal.h` (include the new header)
- Test: `tests/unit/packet_edge_test.cpp`, `tests/unit/scheduler_test.cpp`

**Why declarations go in a public header:** `src/` is a *private* include directory of `ge_core` (`CMakeLists.txt:132`), so no test target can include a `src/*.h`. The functions must be declared where tests can reach them. `include/ge/cpp/input_binding.h` is the right home: it already owns `InputBatch`, and the counter-half of this feature (`event_ports`) lives there too.

**Interfaces:**
- Consumes: `ge::RouteEntry` (`include/ge/cpp/runtime_topology.h:22`), `ge::ConnectionContract` (`include/ge/cpp/capability.h:181`), `ge::JsonValue` (`include/ge/cpp/json.h`), `ge::Metadata` (`include/ge/cpp/packet.h:163`).
- Produces:
  - `bool ge::IsVideoRoute(const ConnectionContract& contract)`
  - `std::optional<PacketSeq> ge::FormatEventMatchSeq(const JsonValue& detail)`
  - `Packet ge::MakeFormatEventPacket(const Packet& keyframe, std::string_view event_type, const JsonValue& detail)`
  - `constexpr std::string_view ge::kFormatEventType` / `kFormatEventSeqKey`

  Task 3 consumes all of these by these exact names.

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/packet_edge_test.cpp` before the closing namespace. It already includes `ge/cpp/packet.h` and uses `ge::TypeTagRegistry`; add `#include <ge/cpp/input_binding.h>` to its include block:

```cpp
TEST(FormatEventTest, MatchSeqRejectsEverythingButAPositiveInteger) {
  using ge::FormatEventMatchSeq;
  EXPECT_FALSE(FormatEventMatchSeq(ge::JsonValue(ge::JsonObject{})).has_value());
  EXPECT_FALSE(FormatEventMatchSeq(ge::JsonValue(ge::JsonObject{
      {"first_key_seq", ge::JsonValue("7")}})).has_value());
  EXPECT_FALSE(FormatEventMatchSeq(ge::JsonValue(ge::JsonObject{
      {"first_key_seq", ge::JsonValue(std::int64_t{0})}})).has_value());
  EXPECT_FALSE(FormatEventMatchSeq(ge::JsonValue(ge::JsonObject{
      {"first_key_seq", ge::JsonValue(std::int64_t{-4})}})).has_value());
  // A non-object detail has no keys at all.
  EXPECT_FALSE(FormatEventMatchSeq(ge::JsonValue(std::int64_t{7})).has_value());
  const auto ok = FormatEventMatchSeq(ge::JsonValue(ge::JsonObject{
      {"first_key_seq", ge::JsonValue(std::int64_t{42})}}));
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(*ok, 42U);
}

TEST(FormatEventTest, EventPacketCopiesTheKeyframeIdentity) {
  ge::Packet key;
  key.header.seq = 9;
  key.header.pts_ns = 1234;
  key.header.dts_ns = 1200;
  key.header.flags = GE_PACKET_FLAG_KEYFRAME;
  key.header.type_tag = ge::TypeTagRegistry::Global().Intern("VideoFrame");
  const ge::JsonValue detail(ge::JsonObject{{"first_key_seq", ge::JsonValue(std::int64_t{9})},
                                            {"pixel_format", ge::JsonValue("NV12")}});
  const ge::Packet ev = ge::MakeFormatEventPacket(key, "media_format_changed", detail);
  EXPECT_TRUE(ev.is_event());
  EXPECT_FALSE(ev.is_eos());
  EXPECT_FALSE((ev.header.flags & GE_PACKET_FLAG_KEYFRAME) != 0);
  EXPECT_EQ(ev.header.seq, 9U);
  EXPECT_EQ(ev.header.pts_ns, 1234);
  EXPECT_EQ(ev.header.dts_ns, 1200);
  EXPECT_EQ(ev.header.type_tag, ge::TypeTagRegistry::Global().Intern("media_format_changed"));
  // Metadata entries are strings, so numbers round-trip through their JSON
  // text; the structured detail travels in the side-band event.
  ASSERT_TRUE(ev.metadata != nullptr);
  const std::string* seq = ev.metadata->Get("first_key_seq");
  ASSERT_NE(seq, nullptr);
  EXPECT_EQ(*seq, "9");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build -j 2>&1 | head -20`
Expected: FAIL to compile — `ge::FormatEventMatchSeq` / `ge::MakeFormatEventPacket` are not declared.

- [ ] **Step 3: Declare the API**

In `include/ge/cpp/input_binding.h`, add to the includes:

```cpp
#include <string_view>
```

(already present) and after the `InputBatch` struct:

```cpp
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
```

Note the deliberate signature: `IsVideoRoute` takes a `ConnectionContract&`, not a `RouteEntry&`. `runtime_topology.h` includes this header (`include/ge/cpp/runtime_topology.h:16`), so naming `RouteEntry` here would close an include cycle. `ConnectionContract` arrives through `edge_channel.h`, which this header already includes, so the contract form needs no new include. Task 3's call site passes `r.contract`.

- [ ] **Step 4: Implement in a new translation unit**

Create `src/format_event.cpp`:

```cpp
#include <ge/cpp/input_binding.h>

#include <memory>
#include <string>

#include <ge/c/ge_abi.h>
#include <ge/cpp/packet.h>
#include <ge/cpp/types.h>

namespace ge {

bool IsVideoRoute(const ConnectionContract& contract) { return contract.video.has_value(); }

std::optional<PacketSeq> FormatEventMatchSeq(const JsonValue& detail) {
  // GetInteger() returns nullopt for a missing key and for a non-number, so
  // there is no separate presence check to get wrong.
  const std::optional<std::int64_t> n = detail.GetInteger(kFormatEventSeqKey);
  if (!n.has_value() || *n <= 0) return std::nullopt;
  return static_cast<PacketSeq>(*n);
}

Packet MakeFormatEventPacket(const Packet& keyframe, std::string_view, const JsonValue& detail) {
  Packet ev;
  ev.header.seq = keyframe.header.seq;
  ev.header.pts_ns = keyframe.header.pts_ns;
  ev.header.dts_ns = keyframe.header.dts_ns;
  ev.header.flags = GE_PACKET_FLAG_EVENT;
  ev.header.type_tag = TypeTagRegistry::Global().Intern(std::string(kFormatEventType));
  // Metadata stores string-valued entries, so the detail is flattened the
  // same way the C ABI path does it (src/plugin_operator.cpp:293).
  if (Result<Metadata> m = Metadata::FromJson(detail.Serialize()); m.ok()) {
    ev.metadata = std::make_shared<const Metadata>(std::move(*m));
  }
  ev.ingress_ns = keyframe.ingress_ns;
  return ev;
}

}  // namespace ge
```

- [ ] **Step 5: Add it to the build**

In `CMakeLists.txt`, add `src/format_event.cpp` to the `ge_core` source list (the list that ends with `src/c_api.cpp` at line 127). Note `input_binding.cpp` is likely already there; place the new file next to it.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build -j && ctest --test-dir build -R ge_packet_edge_test --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Add the video-route predicate test**

Add the `VideoPort` helper to `tests/unit/test_operators.h` under `namespace ge::test`, moved from `tests/unit/builtin_operators_test.cpp:27-37` (that file then includes the shared one instead of keeping a private copy — one definition, not two). Keep `type_tag = "VideoFrame"` and the `p.video = ge::VideoConstraints{}` line: that optional is what `IsVideoRoute` reads. The test itself goes in `tests/unit/scheduler_test.cpp`:

```cpp
TEST(SchedulerTest, OnlyVideoContractsCountAsVideoRoutes) {
  Fixture f;
  ge::GraphBuilder b("video-route");
  auto src = b.AddNode(Op("VSrc@1.0.0"), "src",
                       ge::JsonValue(ge::JsonObject{{"count", ge::JsonValue(std::int64_t{1})}}));
  auto vout = b.AddNode(Op("VSink@1.0.0"), "vout");
  auto bout = b.AddNode(Op("Sink@1.0.0"), "bout");
  b.Connect(src.port("out"), vout.port("in"), {.id = "e0"});
  b.Connect(src.port("bytes"), bout.port("in"), {.id = "e1"});
  auto topo = f.Build(*b.Build());
  ASSERT_TRUE(topo);
  const std::vector<ge::RouteEntry>* video = topo->RoutesFor(topo->FindNode("src")->id(), "out");
  ASSERT_NE(video, nullptr);
  ASSERT_FALSE(video->empty());
  EXPECT_TRUE(ge::IsVideoRoute(video->front().contract));
  const std::vector<ge::RouteEntry>* bytes = topo->RoutesFor(topo->FindNode("src")->id(), "bytes");
  ASSERT_NE(bytes, nullptr);
  ASSERT_FALSE(bytes->empty());
  EXPECT_FALSE(ge::IsVideoRoute(bytes->front().contract));
}
```

This needs `VSrc` (video out, plus a `bytes` output carrying an opaque contract) and `VSink` (video in). Add a shared `RegisterVideoFixture(ge::BuiltinOperatorFactory& factory, std::vector<std::shared_ptr<ge::Operator>>& keep)` to `tests/unit/test_operators.h` in this step and use it here; `scheduler_test.cpp`'s own `Fixture` registers it in its constructor. Task 4 reuses the helper rather than registering a second copy.

- [ ] **Step 8: Run it**

Run: `cmake --build build -j && ctest --test-dir build -R ge_scheduler_test --output-on-failure`
Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add include/ge/cpp/input_binding.h src/format_event.cpp CMakeLists.txt tests/unit/packet_edge_test.cpp tests/unit/scheduler_test.cpp
git commit -m "feat(scheduler): add the in-band format event primitives

IsVideoRoute, FormatEventMatchSeq and MakeFormatEventPacket keep the
mirror rules in one place: which edges are video, which detail key binds
an event to a keyframe, and what the event Packet looks like. Declared in
input_binding.h because src/ is private to ge_core and tests cannot see
it."
```


---

### Task 3: `Sink` mirrors the format event ahead of its keyframe

**Candidate keying (pre-flight ruling):** a staged candidate is bound to the output
port whose keyframe matches it, not to the seq alone. `Sink::Emit` passes its `port`
into the lookup, and only a keyframe emitted on a *video* route of that port consumes
it. Keying on seq alone would replay the event onto a sibling video output that
happens to emit the same seq — exactly the case Task 4 Step 4 exists to prevent.

**Files:**
- Modify: `src/scheduler_internal.h:15-63` (`Scheduler::Sink`)
- Modify: `src/scheduler_invoke.cpp` (nothing — `Sink` is constructed per call already)
- Test: `tests/unit/scheduler_test.cpp`

**Interfaces:**
- Consumes: `ge::IsVideoRoute(const ConnectionContract&)`, `ge::FormatEventMatchSeq`, `ge::MakeFormatEventPacket`, `ge::kFormatEventType` (Task 2); `Scheduler::Park`, `Scheduler::HasParked`, `PacketRouter::PushOne` (existing).
- Produces: `Sink::Publish` now stages mirrorable candidates and `Sink::Emit` injects them. No new public API. Task 6 reads `ge::NodeMetrics::format_events_unmirrored`, which Task 3's `Sink` destructor is what advances.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/test_operators.h`, under `namespace ge::test` (the shared header, so Task 4 does not have to move them later), then use them from `tests/unit/scheduler_test.cpp`. This needs a producer that both publishes a format event and emits its keyframe in the same call, and a collector that records events separately:

```cpp
// Publishes "media_format_changed" bound to the next keyframe's seq, then
// emits that keyframe. |key_every| controls the stride so a test can check
// that only the matching seq is mirrored.
class FormatAnnouncer final : public ge::Operator {
 public:
  explicit FormatAnnouncer(ge::PacketSeq key_seq) : key_seq_(key_seq) {}
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    if (done_) return ge::ProcessResult::kExhausted;
    done_ = true;
    if (req.events != nullptr) {
      ge::JsonObject d;
      d.emplace("first_key_seq", ge::JsonValue(key_seq_));
      d.emplace("pixel_format", ge::JsonValue("NV12"));
      req.events->Publish("media_format_changed", ge::Severity::kInfo, ge::JsonValue(std::move(d)));
    }
    ge::Packet p;
    p.header.seq = key_seq_;
    p.header.pts_ns = 1000;
    p.header.flags = GE_PACKET_FLAG_KEYFRAME;
    p.header.type_tag = ge::TypeTagRegistry::Global().Intern("VideoFrame");
    return req.sink->Emit("out", std::move(p));
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }

 private:
  ge::PacketSeq key_seq_;
  bool done_ = false;
};

// Records events and data separately, in arrival order, with the port each
// arrived on.
class EventAwareCollector final : public ge::Operator {
 public:
  struct Entry {
    ge::PacketSeq seq;
    bool event;
    std::string port;
    ge::TopologyVersion topology_version;
    ge::ParameterVersion parameter_version;
  };
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < req.inputs.size(); ++i) {
      entries.push_back({req.inputs[i]->header.seq, req.inputs[i]->is_event(),
                         std::string(req.input_ports[i]),
                         req.inputs[i]->header.topology_version,
                         req.inputs[i]->header.parameter_version});
    }
    return ge::ProcessResult::kContinue;
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }
  std::vector<Entry> Entries() const {
    std::lock_guard lock(mutex_);
    return entries;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<Entry> entries;
};
```

Then the test:

```cpp
TEST(SchedulerTest, FormatEventArrivesBeforeItsKeyframe) {
  auto pool = ge::HostBufferPool::Create();
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  EventAwareCollector* sink = nullptr;
  const ge::PortCapability out = VideoPort("out", ge::PortDirection::kOutput, {"NV12"},
                                           ge::PortCardinality::kMulti);
  const ge::PortCapability in = VideoPort("in", ge::PortDirection::kInput, {"NV12"});
  factory.Register(Desc("Announce@1.0.0", {}, {out}, true, 1),
                   [&](const ge::OperatorCreateArgs&) {
                     auto op = std::make_shared<FormatAnnouncer>(7);
                     keep.push_back(op);
                     return op;
                   });
  factory.Register(Desc("Collect@1.0.0", {in}, {}, false, 1),
                   [&](const ge::OperatorCreateArgs&) {
                     auto op = std::make_shared<EventAwareCollector>();
                     sink = op.get();
                     keep.push_back(op);
                     return op;
                   });
  ge::GraphBuilder b("format-frontier");
  auto src = b.AddNode(Op("Announce@1.0.0"), "announce");
  auto col = b.AddNode(Op("Collect@1.0.0"), "collect");
  b.Connect(src.port("out"), col.port("in"), {.id = "e0"});
  auto r = ge::RuntimeTopology::Build(*b.Build(), factory, {.session_id = 11, .version = 1});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto topo = *r;
  ge::ExecutorPool exec(0);
  ge::Scheduler s(topo, exec);
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  while (exec.RunPending()) {
  }
  ASSERT_NE(sink, nullptr);
  const std::vector<EventAwareCollector::Entry> got = sink->Entries();
  ASSERT_EQ(got.size(), 2U) << "expected the event Packet and then the keyframe";
  EXPECT_TRUE(got[0].event);
  EXPECT_EQ(got[0].seq, 7U);
  EXPECT_EQ(got[0].port, "in");
  EXPECT_FALSE(got[1].event);
  EXPECT_EQ(got[1].seq, 7U);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build -j && ctest --test-dir build -R "SchedulerTest.FormatEventArrivesBeforeItsKeyframe" --output-on-failure`
Expected: FAIL — `got.size()` is `1U` (only the keyframe): nothing mirrors the side-band event today.

- [ ] **Step 3: Stage candidates in `Sink::Publish`**

`src/scheduler_internal.h` already includes `<ge/cpp/scheduler.h>`, which pulls in `runtime_topology.h`; `input_binding.h` comes in through that chain too. Confirm the new declarations are visible (add `#include <ge/cpp/input_binding.h>` to `scheduler_internal.h` if not) before relying on them.

Replace `Sink::Publish` with:

```cpp
  void Publish(std::string_view type, Severity severity, JsonValue detail) override {
    if (!active_) return;
    // EVT-4: a format event that names the keyframe it belongs to is also
    // mirrored into the data plane. Staging happens here, on the same
    // thread and inside the same Process call that will emit the keyframe;
    // an event published outside a call (open/close) never reaches this
    // function at all, so no scope bookkeeping is needed.
    const std::optional<PacketSeq> seq = FormatEventMatchSeq(detail);
    if (type == kFormatEventType && seq.has_value()) {
      pending_.push_back(PendingFormatEvent{*seq, detail});
    }
    // The side-band publication is unchanged and must stay exactly one per
    // call: it is the observation contract (12 §4.5), not a data-plane
    // delivery.
    if (!s_.events_.on_operator_event) return;
    s_.events_.on_operator_event(node_, std::string(type), severity, std::move(detail));
  }
```

Add the pending type and member inside `Sink` (private):

```cpp
  struct PendingFormatEvent {
    PacketSeq seq;
    JsonValue detail;
  };
  std::vector<PendingFormatEvent> pending_;
```

Staging is keyed on the seq the producer named; the *port* it belongs to is the port
that emits the matching keyframe, so no port is recorded here.

- [ ] **Step 4: Add the diagnostic counter**

Step 7's negative-path test reads it, so it lands here rather than later. In
`include/ge/cpp/node_runtime.h`, next to `orphan_completions` (`:125`):

```cpp
  // EVT-4: staged format events that never found their binding keyframe.
  // Side-band publication is unaffected; this is the diagnostic count.
  std::atomic<std::uint64_t> format_events_unmirrored{0};
```

- [ ] **Step 5: Inject in `Sink::Emit`**

Replace `Sink::Emit` with a version that mirrors first, through the identical per-edge path. Factor the existing loop body into a lambda so the two packets cannot drift:

```cpp
  Status Emit(std::string_view port, Packet packet) override {
    if (!active_) return Status::InvalidArgument("emit outside of process");
    node_.NoteSeq(packet.header.seq);
    // EVT-4: mirror the staged format event, if any, immediately before
    // the keyframe it binds to. Both packets go through the same per-edge
    // decision, so a full block edge parks them in FIFO order and the
    // downstream never sees the keyframe first.
    const bool keyframe = (packet.header.flags & GE_PACKET_FLAG_KEYFRAME) != 0;
    const std::vector<RouteEntry>* routes = keyframe ? topo_.RoutesFor(node_.id(), port) : nullptr;
    // A keyframe only consumes a candidate when this port actually carries a
    // video route: a data keyframe on an opaque output must not swallow an
    // event bound to a video keyframe that has not been emitted yet, and a
    // sibling video port must not replay it.
    const bool video_port =
        routes != nullptr && std::any_of(routes->begin(), routes->end(),
                                         [](const RouteEntry& r) { return IsVideoRoute(r.contract); });
    if (keyframe && video_port && !pending_.empty()) {
      const auto it = std::find_if(pending_.begin(), pending_.end(),
                                   [&](const PendingFormatEvent& p) { return p.seq == packet.header.seq; });
      if (it != pending_.end()) {
        const JsonValue detail = it->detail;
        pending_.erase(it);  // one event Packet per keyframe, never two
        if (const Status s = PushOnRoutes(port, MakeFormatEventPacket(packet, kFormatEventType, detail), true);
            !s.ok()) {
          return s;
        }
      }
    }
    return PushOnRoutes(port, std::move(packet), false);
  }

  // Records that a staged event never found its keyframe. Called once per
  // leftover when the call ends; the side-band copy already went out.
  ~Sink() {
    if (pending_.empty()) return;
    for (const PendingFormatEvent& p : pending_) {
      node_.metrics().format_events_unmirrored.fetch_add(1, std::memory_order_relaxed);
    }
  }
```

and the shared push path:

```cpp
 private:
  Status PushOnRoutes(std::string_view port, Packet packet) {
    Result<PacketRef> shared = PacketRouter::Prepare(topo_, node_, port, std::move(packet), pv_);
    if (!shared.ok()) return shared.status();
    if (!*shared) return Status::Ok();
    const std::vector<RouteEntry>* routes = topo_.RoutesFor(node_.id(), port);
    EmitReport report;
    for (const RouteEntry& r : *routes) {
      if (!IsVideoRoute(r.contract)) continue;  // audio/tensor/opaque legs get the keyframe only
      if (s_.HasParked(node_, *r.edge)) {
        s_.Park(node_, *r.edge, *shared);
        continue;
      }
      if (PacketRouter::PushOne(*r.edge, *shared, node_, &report) == PushOutcome::kWouldBlock) {
        s_.Park(node_, *r.edge, *shared);
      }
    }
    s_.AfterEmit(report);
    return Status::Ok();
  }
```

Note the `continue` on non-video routes: for the keyframe itself this must **not** skip — see Step 6.

- [ ] **Step 6: Keep the keyframe's fan-out complete**

The predicate in Step 5 is right for the event and wrong for the keyframe: the keyframe must still reach every edge, video or not. Give `PushOnRoutes` a flag and pass `false` for the keyframe:

```cpp
  Status PushOnRoutes(std::string_view port, Packet packet, bool video_only) {
    Result<PacketRef> shared = PacketRouter::Prepare(topo_, node_, port, std::move(packet), pv_);
    if (!shared.ok()) return shared.status();
    if (!*shared) return Status::Ok();
    const std::vector<RouteEntry>* routes = topo_.RoutesFor(node_.id(), port);
    EmitReport report;
    for (const RouteEntry& r : *routes) {
      if (video_only && !IsVideoRoute(r.contract)) continue;  // audio/tensor/opaque legs get the keyframe only
      if (s_.HasParked(node_, *r.edge)) {
        s_.Park(node_, *r.edge, *shared);
        continue;
      }
      if (PacketRouter::PushOne(*r.edge, *shared, node_, &report) == PushOutcome::kWouldBlock) {
        s_.Park(node_, *r.edge, *shared);
      }
    }
    s_.AfterEmit(report);
    return Status::Ok();
  }
```

with the two call sites reading `PushOnRoutes(port, MakeFormatEventPacket(...), /*video_only=*/true)` and `PushOnRoutes(port, std::move(packet), /*video_only=*/false)`.

Both packets go through `Prepare` and therefore through `PushEdge` — the same `EmitReport` accounting the data path uses, and the same `packets_out` exclusion for control packets (`src/runtime_topology.cpp:247`).

- [ ] **Step 7: Run the tests to verify they pass**

Run: `cmake --build build -j && ctest --test-dir build -R ge_scheduler_test --output-on-failure`
Expected: PASS, including the whole existing `SchedulerTest` suite (the keyframe fan-out change is what those tests police).

- [ ] **Step 8: Add the negative-path test**

Append a test asserting the event is *not* mirrored when the binding is wrong, and that the side-band copy still arrives:

```cpp
TEST(SchedulerTest, UnboundFormatEventStaysSideBandOnly) {
  // Same harness as above, but the producer publishes first_key_seq = 7
  // while emitting a keyframe with seq = 8, and subscribes to the session's
  // EventBus through the fixture's on_operator_event hook.
  // Expectations:
  //   EXPECT_EQ(sink->Entries().size(), 1U);           // keyframe only
  //   ASSERT_EQ(observed_events.size(), 1U);           // side-band still out
  //   EXPECT_EQ(observed_events[0].type, "media_format_changed");
  //   EXPECT_EQ(announcer_metrics.format_events_unmirrored.load(), 1U);
}
```

Fill it in with the same construction as Step 1, passing a `SchedulerEvents{.on_operator_event = ...}` that appends to `observed_events`, and a `FormatAnnouncer` constructed with `key_seq = 8` whose published detail still says `first_key_seq = 7`. Parameterize `FormatAnnouncer` with a second `detail_seq` member so both tests share one class instead of two, and add a third case in the same test that covers Review Focus #6 precisely: the detail binds to seq 7, the node emits seq 7 as *data* (no `GE_PACKET_FLAG_KEYFRAME`). Then assert the event is not mirrored even though a seq-7 packet exists — the binding is keyframe-specific, not seq-specific — that the side-band copy still arrives, and that `format_events_unmirrored` advanced once when the call ended.

- [ ] **Step 9: Run it**

Run: `cmake --build build -j && ctest --test-dir build -R ge_scheduler_test --output-on-failure`
Expected: PASS.

- [ ] **Step 10: Commit**

```bash
git add src/scheduler_internal.h include/ge/cpp/node_runtime.h tests/unit/scheduler_test.cpp
git commit -m "feat(scheduler): mirror format events ahead of their keyframe

Sink::Publish stages a media_format_changed candidate whose detail names
a keyframe; Sink::Emit pushes the event Packet through the same per-edge
park/push path immediately before that keyframe, so Edge FIFO order holds
even when the edge is full. Unmatched candidates are counted, not
silently dropped, and the side-band publication is unchanged."
```

---

### Task 4: Fan-out, mixed outputs, and failure-mode coverage

**Files:**
- Test: `tests/unit/scheduler_test.cpp`
- Modify: `tests/unit/test_operators.h` (add the shared video fixture used here and by Task 2 Step 5)

**Interfaces:**
- Consumes: everything Task 3 produced; the `FormatAnnouncer` / `EventAwareCollector` pair from Task 3 Step 1 (move both into `tests/unit/test_operators.h` in this task so other suites can reuse them, and update Task 3's tests to include the header).
- Produces: no production code. This task is the coverage half of the spec's §测试与验收.

- [ ] **Step 1: Extend the shared helper**

`FormatAnnouncer`, `EventAwareCollector`, `VideoPort` and `RegisterVideoFixture` already live in `tests/unit/test_operators.h` (Tasks 2 and 3 put them there). This task only extends `FormatAnnouncer` for the cases below and adds the one registration helper the fan-out tests share:

```cpp
// Registers a video-formatted producer/consumer pair for EVT-4 tests. The
// ports carry VideoConstraints so the negotiated contract has a video
// format and IsVideoRoute() is true, with no media build required.
// |detail_seq| differs from the announced keyframe seq in the negative
// cases; |repeat| publishes the same detail more than once.
inline void RegisterFormatEventPair(ge::BuiltinOperatorFactory& factory,
                                    std::vector<std::shared_ptr<ge::Operator>>& keep,
                                    EventAwareCollector** sink, ge::PacketSeq key_seq,
                                    ge::PacketSeq detail_seq, int repeat = 1);
```

- [ ] **Step 2: Write the fan-out test (Review Focus #5)**

```cpp
TEST(FormatEventTest, EveryVideoEdgeSeesEventThenKeyframeEvenWhenOneIsFull) {
  // src --e0(capacity 1, kBlock)--> slow   (Collect's Process blocks on a Gate
  // src --e1(capacity 64)---------> fast
  // Both collectors must record [event(7), keyframe(7)] in that order.
  // The slow branch is released only after the announcer has emitted, so
  // its edge is provably full when the event and keyframe are pushed.
  // Assertions per collector:
  //   ASSERT_EQ(entries.size(), 2U);
  //   EXPECT_TRUE(entries[0].event);  EXPECT_EQ(entries[0].seq, 7U);
  //   EXPECT_FALSE(entries[1].event); EXPECT_EQ(entries[1].seq, 7U);
}
```

Use `Gate` (already in `test_operators.h`) on the slow branch: it blocks in `Process` until `Release()`, so the edge fills deterministically instead of racing a sleep. Release it after `Announce` has run (wait on `entered`) and before draining.

- [ ] **Step 3: Write the mixed-output test (video vs. opaque)**

```cpp
TEST(FormatEventTest, NonVideoEdgeReceivesTheKeyframeOnly) {
  // The announcer has two outputs: "out" (video contract) and "bytes"
  // (opaque Bytes contract). One collector hangs off each.
  // Assertions:
  //   video_entries  == [event(7), keyframe(7)]
  //   bytes_entries  == [keyframe(7)]
  //   EXPECT_FALSE(bytes_entries[0].event);
}
```

- [ ] **Step 4: Write the sibling-video-output test (Review Focus #2)**

The mixed-output test covers video vs. opaque. This one covers the harder pair: *two* video outputs on the same node, where the format change belongs to only one. A naive implementation that keys the mirror on "any keyframe with this seq" rather than on the emitting port would leak the event onto the sibling.

```cpp
TEST(FormatEventTest, SiblingVideoOutputDoesNotGetTheEvent) {
  // The announcer has "out" and "out2", both with VideoConstraints. It
  // publishes media_format_changed bound to seq 7, then emits a keyframe
  // at seq 7 on "out" ONLY (and a keyframe at seq 99 on "out2").
  // Assertions:
  //   out_entries  == [event(7), keyframe(7)]
  //   out2_entries == [keyframe(99)]     // no event Packet at all
}
```

This is also what pins the "the event belongs to the output port that emitted the matched keyframe" half of the contract: with two video legs, the pending candidate must be consumed by the first matching emit and must not be replayed onto the second.

- [ ] **Step 5: Write the duplicate-publication test (Review Focus #4)**

```cpp
TEST(FormatEventTest, DuplicatePublicationMirrorsOnce) {
  // The announcer publishes the same first_key_seq twice before emitting
  // one keyframe.
  // Assertions:
  //   ASSERT_EQ(entries.size(), 2U) << "one event Packet, not two";
  //   EXPECT_TRUE(entries[0].event);
  //   EXPECT_FALSE(entries[1].event);
}
```

Make `FormatAnnouncer` publish `repeat_` times (default 1) and emit to a configurable output port list, so Tasks 3 and 4 share one class.

- [ ] **Step 6: Write the mutation-race test (Review Focus #1)**

```cpp
TEST(FormatEventTest, PendingEventDoesNotCrossATopologySwap) {
  // Publish a format event bound to seq 7 but emit no keyframe in that
  // call; then Publish a topology that removes the consumer and drain it.
  // Assertions:
  //   - the first call's side-band event was delivered exactly once;
  //   - the removed consumer received no event Packet;
  //   - the node's format_events_unmirrored count advanced once;
  //   - the new topology's consumer, once it gets a keyframe at seq 7 in a
  //     later call, receives event(7) then keyframe(7).
}
```

Drive the swap with the scheduler's `Publish(next, RetireRequest{...})` as the existing mutation tests in this file do; do not add new mutation machinery.

- [ ] **Step 7: Write the outside-scope plugin test (Review Focus #3, first half)**

Do not construct a real `.so`. Build a `HostApiAdapter` directly, which is what `PluginOperator` does (`src/plugin_operator.cpp:147`). This step owns only the path that must not change; Task 5 owns the in-scope path, which is where that code actually changes:

```cpp
TEST(FormatEventTest, PluginPublishOutsideAScopeStaysSideBandOnly) {
  // Construct HostApiAdapter with HostServices capturing the published
  // events. With no InvokeScope active, event_publish must publish
  // side-band exactly once, with the same fields the pre-change code
  // produced (session, node, severity, detail), and must not create an
  // in-band Packet (a recording Sink stands ready and records zero emits).
}
```

- [ ] **Step 8: Write the alignment-non-participation test (spec §顺序与失败语义 4)**

A mirrored event must not stand in for a packet on a required port, must not move the `kAligned` window, and must not trigger `kLatest`. The binding code already behaves this way (`src/input_binding.cpp` leaves control packets out of every sync decision) — this test is what keeps Task 3's injection from silently breaking it.

```cpp
TEST(FormatEventTest, MirroredEventDoesNotSatisfyASecondRequiredPort) {
  // Consumer with two required inputs, "video" and "ref", policy kAligned
  // with a 40ms window. Feed "ref" one packet and nothing else, then let
  // the announcer emit its event+keyframe on "video".
  // Assertions:
  //   - the consumer's Process is called exactly once for the keyframe at
  //     seq 7 (the event and the keyframe arrive in the same batch);
  //   - no batch is ever produced from the event alone while "ref" is
  //     empty — a counter of zero-input calls stays at end (the collector
  //     records every call, so assert the call count, not just the packets);
  //   - the "ref" packet is still delivered, i.e. the alignment window did
  //     not advance past it.
}
```

Also cover `kLatest`: with a `kLatest` consumer, a mirrored event must not be the packet that triggers the invocation when a data packet is available.

- [ ] **Step 9: Extend the mutation-race test with version assertions (spec §顺序与失败语义 6)**

In Step 6's test, add to the assertions:

```cpp
  //   - the mirrored event and its keyframe carry the same
  //     topology_version as the keyframe's own header (the router stamps
  //     both in Prepare, so a mismatch means the mirror bypassed it);
  //   - no event Packet reaches the new topology's consumer with the old
  //     version.
```

Read the version off the recorded `EventAwareCollector::Entry` (add a `ge::TopologyVersion topology_version` field and a `ge::ParameterVersion parameter_version` field to `Entry`, populated from the input packets — the collector already sees both in `req`). Asserting on the packet headers is the point: if the mirror built its Packet without going through `PacketRouter::Prepare`, these fields would be `0` and the test fails.

- [ ] **Step 10: Run the new tests**

Run: `cmake --build build -j && ctest --test-dir build -R "ge_scheduler_test|ge_packet_edge_test" --output-on-failure`
Expected: PASS. Debug the mirroring, not the tests: a failing order assertion here means `PushOnRoutes`' two calls drifted apart.

- [ ] **Step 11: Run the whole non-stress suite**

Run: `ctest --test-dir build --output-on-failure -LE "stress|soak"`
Expected: 35/35 PASS (the existing baseline) plus the new cases.

- [ ] **Step 12: Commit**

```bash
git add tests/unit/test_operators.h tests/unit/scheduler_test.cpp
git commit -m "test(scheduler): cover format event frontier fan-out and failure modes

Fan-out with a full block edge, mixed video/opaque outputs, duplicate
publication, topology swap with a staged event (including version
fields), alignment non-participation, and the plugin entry both inside
and outside an InvokeScope."
```

---

### Task 5: Unify the plugin entry through the active `EventSink`

**Files:**
- Modify: `src/plugin_operator.cpp:1-30` (the thread-local `Invoke` struct), `:163-180` (`InvokeScope`), `:377-385` (`EventPublish`)
- Modify: `include/ge/cpp/plugin_operator.h:45-60` (`InvokeScope`)
- Test: `tests/unit/builtin_operators_test.cpp` or a new `tests/unit/plugin_event_test.cpp` — declare it with `ge_add_unit_test`

**Interfaces:**
- Consumes: `ge::EventSink` (`include/ge/cpp/operator.h:59`); Task 4's plugin test doubles.
- Produces: `HostApiAdapter::InvokeScope(HostApiAdapter& adapter, EmitSink& sink, EventSink* events)` — the three-argument form. Existing two-argument call sites must be updated in this task; there is exactly one (`src/plugin_operator.cpp`, `PluginOperator::Process`). After this task, an event published by a plugin inside a process call reaches `Sink::Publish` and is therefore mirrorable.

- [ ] **Step 1: Write the failing test**

Add a case that asserts the plugin path shares the operator path's outcome rather than duplicating it: publish through `host_api.event_publish` inside a scope whose `EventSink` is a real `Scheduler::Sink`, emit the matching keyframe through that same scope, and expect the same `[event, keyframe]` ordering a built-in producer produces.

Write the test against the adapter's public surface (`vtable()`, `InvokeScope`, `Fill`) plus the session's `HostServices` — never by reaching into private members. Two assertions, both against a recording `EventSink` standing in for the scheduler's:

```cpp
TEST(PluginEventTest, PublishInsideScopeReachesTheCallingEventSink) {
  // (a) With an InvokeScope open over a recording EventSink, call
  //     ge_host_api::event_publish with a "media_format_changed" ge_event
  //     whose detail_json is {"first_key_seq":7}. The recording sink sees
  //     Publish("media_format_changed", …) exactly once, and the detail it
  //     receives is the parsed JSON (GetInteger("first_key_seq") == 7) —
  //     not the raw string, which is what pins the ParseJson conversion.
  // (b) With no scope active, the same call reaches HostServices::event_publish
  //     exactly once and the recording sink sees nothing.
}
```

The end-to-end ordering claim (`[event(7), keyframe(7)]` through a real `Scheduler::Sink`) belongs to Task 4's harness, which already stands up that graph; standing one up here would duplicate it. If an end-to-end plugin case is cheap to add after (a) and (b) pass, add it — but do not let it block this task.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build -j && ctest --test-dir build -R ge_plugin_event_test --output-on-failure`
Expected: FAIL — only the keyframe arrives; the plugin's event bypassed `Sink` and went straight to the session services.

- [ ] **Step 3: Carry the sink into the scope**

In `src/plugin_operator.cpp`, extend the thread-local:

```cpp
struct Invoke {
  HostApiAdapter* adapter = nullptr;
  EmitSink* sink = nullptr;
  // EVT-4: the event sink of the same call, so a plugin's event_publish
  // goes through the same path as a built-in operator's req.events and can
  // be mirrored in-band. Null for a scope that has no scheduler behind it.
  EventSink* events = nullptr;
  bool exhausted = false;
};
```

In `include/ge/cpp/plugin_operator.h`, change the scope constructor to take the event sink and store it on the adapter's current invoke:

```cpp
    InvokeScope(HostApiAdapter& adapter, EmitSink& sink, EventSink* events = nullptr) noexcept;
```

and in the `.cpp`, set `inv->events = events;` alongside `inv->sink = &sink;`.

- [ ] **Step 4: Route `EventPublish` through it when a scope is active**

```cpp
ge_status HostApiAdapter::EventPublish(const ge_event* event) {
  if (!HeaderOk(event, sizeof(ge_event)) || event->type == nullptr) {
    return MakeStatus(Status::InvalidArgument("invalid event"));
  }
  // Inside a process call the event belongs to the call's node: hand it to
  // the scheduler's event sink, which publishes the side-band copy and (for
  // a bound format event) stages the in-band mirror. Outside a call --
  // open/close, or an async worker after the call returned -- there is no
  // such sink, and the session services below stay the only path.
  if (Invoke* inv = g_current_invoke; inv != nullptr && inv->events != nullptr) {
    JsonValue detail(JsonObject{});
    if (event->detail_json != nullptr && event->detail_json[0] != '\0') {
      JsonParseResult parsed = ParseJson(event->detail_json);
      if (parsed.ok()) detail = std::move(*parsed.value);
    }
    inv->events->Publish(event->type, static_cast<Severity>(event->severity), std::move(detail));
    return OkStatus();
  }
  const HostServices services = SessionServices::Global().Find(event->session_id);
  if (!services.event_publish) return MakeStatus(Status::NotFound("no event sink for session"));
  services.event_publish(*event);
  return OkStatus();
}
```

Two conversions are the compatibility yardstick and both already exist: `ParseJson` returning a `JsonParseResult` (`include/ge/cpp/json.h:227`, `.value` is a `std::optional<JsonValue>`), and the severity mapping that `src/engine.cpp:232-236` performs when it converts a `ge_event` into a `ge::Event`. Match that mapping (including how it assigns `Event::node` and `Event::session`) rather than inventing one here.

Note that `HostApiAdapter::EventPublish` previously did not parse the detail at all — it forwarded the `ge_event` struct and let `Engine` parse it. This step moves the parse earlier for the in-scope path only. The Task 4 outside-scope test is what proves the second path is byte-for-byte unchanged.

- [ ] **Step 5: Register the new test target**

`tests/unit/plugin_event_test.cpp` is a new file; CMake needs it registered or the test silently never runs. Check whether `tests/CMakeLists.txt` globs (`ge_add_unit_test (ge_x unit/x.cpp)` style — the file lists each target explicitly), and if it does, add:

```cmake
ge_add_unit_test (ge_plugin_event_test unit/plugin_event_test.cpp)
```

next to `ge_builtin_operators_test` (line 98). Then re-run `cmake -S . -B build` so the target exists; `ctest -N -R ge_plugin_event_test` must list it before Step 6's run means anything.

- [ ] **Step 6: Update the one call site**

In `src/plugin_operator.cpp`, `PluginOperator::Process` constructs the scope at line ~472. It has access to the request, so thread the event sink through:

```cpp
  HostApiAdapter::InvokeScope scope(*adapter_, *request.sink, request.events);
```

- [ ] **Step 7: Run the tests**

Run: `cmake --build build -j && ctest --test-dir build -R "ge_plugin_event_test|ge_scheduler_test|ge_builtin_operators_test" --output-on-failure`
Expected: PASS, with the side-band copy delivered exactly once on both paths (the Task 4 plugin test asserts the outside-scope path is unchanged).

- [ ] **Step 8: Commit**

```bash
git add include/ge/cpp/plugin_operator.h src/plugin_operator.cpp tests/CMakeLists.txt tests/unit/plugin_event_test.cpp
git commit -m "feat(plugin): route plugin events through the calling scheduler

A plugin's event_publish inside a process call now reaches the same
EventSink a built-in operator uses, so a bound media_format_changed is
mirrored in-band. Outside a call the session-services path is unchanged."
```---

### Task 6: Surface the un-mirrored counter

**Files:**
- Modify: `src/metrics_export.cpp:190-210` (node metric block)
- Test: `tests/unit/metrics_export_test.cpp`

**Interfaces:**
- Consumes: `ge::NodeMetrics::format_events_unmirrored` (Task 3 Step 8).
- Produces: the Prometheus series `node_format_events_unmirrored_total`, which is what the spec's "递增未镜像格式事件计数并写诊断" requirement is measurable through.

- [ ] **Step 1: Write the failing test**

Add to `tests/unit/metrics_export_test.cpp`, following the file's existing "render and grep the text" pattern:

```cpp
TEST(MetricsExportTest, UnmirroredFormatEventsAreExported) {
  // Render a topology containing one node and assert the output contains
  //   node_format_events_unmirrored_total{
  // with the node's label. Set the counter to 3 through the node's metrics
  // before rendering and assert the value is 3.
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build -j && ctest --test-dir build -R ge_metrics_export_test --output-on-failure`
Expected: FAIL — the series is absent.

- [ ] **Step 3: Add the series**

In `src/metrics_export.cpp`, in the per-node block (next to `node_would_block_total`):

```cpp
    w.Counter("node_format_events_unmirrored_total",
              "Format events published without a binding keyframe (side-band only)", nl,
              m.format_events_unmirrored.load(std::memory_order_relaxed));
```

- [ ] **Step 4: Run it**

Run: `cmake --build build -j && ctest --test-dir build -R ge_metrics_export_test --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/metrics_export.cpp tests/unit/metrics_export_test.cpp
git commit -m "feat(metrics): export un-mirrored format event count

A format event whose first_key_seq never matches a keyframe is published
side-band only; the counter makes that visible instead of silent."
```

---

### Task 7: Documentation sync

**Files:**
- Modify: `docs/12-详细设计.md:449-450` (the in-band event contract, which currently promises the injection and defers consumption to "P7")
- Modify: `docs/12-详细设计.md:993` (`EventKind` note)
- Modify: `docs/08-场景-运行期事件通知.md:52-60` (EV-M-3/4/7 status)
- Modify: `docs/13-接口与类设计.md` (the `Scheduler::Sink` row and the `InputBatch` field list, if `InputBatch` appears in the catalogue)
- Test: `scripts/check_docs_symbols.py` via `ctest -R ge_docs_symbols`

**Interfaces:**
- Consumes: the shipped code from Tasks 1-6; the symbols it introduces (`FormatEventMatchSeq`, `IsVideoRoute`, `MakeFormatEventPacket` are declared in `include/ge/cpp/input_binding.h` and defined in `src/format_event.cpp`, both of which the guard's scan covers).
- Produces: docs that no longer describe the pre-EVT-4 state.

- [ ] **Step 1: Update `docs/12` §4.5**

Replace the two bullets at lines 449-450 so they describe what now ships, and say plainly which part is deferred:

```markdown
- 产生格式事件的 Node 在发布 `media_format_changed` 的同时，把事件镜像为带 `GE_PACKET_FLAG_EVENT` 的控制 Packet，插入其绑定的关键帧之前（`detail.first_key_seq` 即绑定键）；每个视频输出 Edge 各自按自身 FIFO 排在对应关键帧前，音频与自定义数据 Edge 只收关键帧。旁路 EventBus 副本保持一份不变。
- InputBinding 遇到事件 Packet 时不参与同步，随下一次 acquire 或 event-only batch 交付；`InputBatch.event_ports` 与 `events` 平行，下游据此判定事件来自哪条输入流。事件与关键帧携带同一 `topology_version`/`parameter_version`，不跨拓扑版本。
- 专用 `on_event` 回调仍属 ABI v2 范围：本期事件以普通 `ProcessRequest.inputs` 交付，现有算子跳过 event 的行为不变。
```

- [ ] **Step 2: Update the `EventKind` note**

At `docs/12:993`, mark `data_plane` as a reserved value rather than an in-use one:

```cpp
enum class EventKind : uint8_t { data_plane, observation };  // data_plane 预留：带内交付用 GE_PACKET_FLAG_EVENT 表达
```

- [ ] **Step 3: Update `docs/08` §5.2 status**

Append a status note under the EV-M table recording which requirements this change satisfies (EV-M-3, EV-M-4, EV-M-7) and which remain open because they need a consumer that reconfigures (EV-M-5, EV-M-6, EV-M-8). Do not mark EV-M-5/6 done — the spec explicitly puts downstream re-negotiation out of scope.

- [ ] **Step 4: Update the class catalogue**

In `docs/13` §7.1/7.2, add `FormatEventMatchSeq` and `IsVideoRoute` (and `MakeFormatEventPacket` if the table lists free functions) with the file `src/format_event.cpp`, and update the `Scheduler::Sink` row to mention the mirror. The guard only checks the first column against `include/` and `src/`, so a name that does not exist fails the suite — which is the point.

- [ ] **Step 5: Run the docs guard**

Run: `ctest --test-dir build -R ge_docs_symbols --output-on-failure`
Expected: PASS. A failure prints the offending symbol and its docs/13 line; fix the doc, not the guard.

- [ ] **Step 6: Run the full suite and commit**

```bash
cmake --build build -j && ctest --test-dir build --output-on-failure -LE "stress|soak"
git add docs/12-详细设计.md docs/08-场景-运行期事件通知.md docs/13-接口与类设计.md
git commit -m "docs: sync the in-band format event contract with the implementation

12 §4.5 now describes the shipped mirroring instead of promising it,
EventKind::data_plane is marked reserved, and docs/08 records that
EV-M-3/4/7 are satisfied while the reconfiguration requirements stay
open."
```

---

## Final verification (after all tasks)

- [ ] `cmake --build build -j` — clean, no new warnings (the repo builds with `-Wshadow` under GCC; rename any lambda parameter that shadows an outer pointer, as `src/c_api.cpp:378` did).
- [ ] `ctest --test-dir build --output-on-failure -LE "stress|soak"` — 35/35 baseline plus the new cases, `ge_docs_symbols` included.
- [ ] Sanitizer lane: `cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DGE_ENABLE_SANITIZERS=ON && cmake --build build/asan -j && ctest --test-dir build/asan --output-on-failure -LE "stress|soak"` — the `Sink` destructor and the `pending_` vector are per-call and single-threaded, so this should be clean; a leak here means a candidate holds a `JsonValue` past its call.
- [ ] TSan lane for `ge_scheduler_test` and `ge_async_runtime_test` — the plugin-path change touches `g_current_invoke`, which is `thread_local`; confirm no new race.
- [ ] Confirm `git diff --stat` against the plan's file list: `src/media/*` untouched, `include/ge/c/*` untouched.
