#include <ge/cpp/plugin_operator.h>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ge/cpp/graph_spec.h>
#include <ge/cpp/scheduler.h>

#include "test_operators.h"

// Task 5 (EVT-4): a plugin's ge_host_api::event_publish inside a process
// call must reach the same EventSink a built-in operator's req.events uses,
// so its format events become mirrorable. Outside a call the pre-change
// session-services path is untouched (the outside-scope Task 4 test in
// scheduler_test.cpp pins that half's field-for-field forwarding).
//
// Everything here drives the adapter's public surface (vtable(),
// InvokeScope, the HostServices hooks) -- no .so is constructed and no
// private member is reached into.

namespace {

using namespace ge::test;

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

// Stands in for the scheduler's Sink on the EventSink side of one call.
class RecordingEventSink final : public ge::EventSink {
 public:
  struct Entry {
    std::string type;
    ge::Severity severity;
    ge::JsonValue detail;
  };
  void Publish(std::string_view type, ge::Severity severity, ge::JsonValue detail) override {
    entries.push_back(Entry{std::string(type), severity, std::move(detail)});
  }
  std::vector<Entry> entries;
};

// InvokeScope needs an EmitSink; these tests never emit.
class NullEmitSink final : public ge::EmitSink {
 public:
  ge::Status Emit(std::string_view, ge::Packet) override { return ge::Status::Ok(); }
};

// The session-services recorder: what the pre-change path (and still the
// outside-scope path) delivers to.
struct HostRecorder {
  int count = 0;
  std::string type;
  std::string detail;
};

ge::HostServices RecordingServices(HostRecorder* rec) {
  ge::HostServices services;
  services.event_publish = [rec](const ge_event& e) {
    ++rec->count;
    rec->type = e.type != nullptr ? e.type : "";
    rec->detail = e.detail_json != nullptr ? e.detail_json : "";
  };
  return services;
}

ge_event FormatEvent(ge::SessionId session, ge::NodeId node, const char* detail_json) {
  ge_event ev{};
  ev.header.struct_size = sizeof(ge_event);
  ev.header.abi_major = GE_ABI_MAJOR;
  ev.kind = GE_EVENT_OBSERVATION;
  ev.type = "media_format_changed";
  ev.severity = GE_SEVERITY_WARNING;  // not the default: pins the cast, not a constant
  ev.session_id = session;
  ev.source_node_id = node;
  ev.detail_json = detail_json;
  return ev;
}

// (a) In scope: the publish lands on the call's EventSink exactly once, with
//     the event type, the mapped severity, and the detail as *parsed JSON*
//     (GetInteger, not a raw string) -- which pins the ParseJson conversion
//     that used to live in Engine. The session-services recorder stays cold.
// (b) Out of scope: the same call reaches HostServices::event_publish
//     exactly once and the recording sink sees nothing new.
TEST(PluginEventTest, PublishInsideScopeReachesTheCallingEventSink) {
  HostRecorder rec;
  ge::HostApiAdapter adapter(/*session=*/77, /*node=*/5, "announce", /*is_source=*/true,
                             RecordingServices(&rec));
  RecordingEventSink events;
  NullEmitSink emits;
  const ge_event ev = FormatEvent(77, 5, R"({"first_key_seq":7})");

  {
    ge::HostApiAdapter::InvokeScope scope(adapter, emits, &events);
    const ge_status st = ge::HostApiAdapter::vtable().event_publish(&ev);
    EXPECT_EQ(st.code, GE_STATUS_OK);
  }
  ASSERT_EQ(events.entries.size(), 1U) << "in scope, the call's EventSink gets it exactly once";
  EXPECT_EQ(events.entries[0].type, "media_format_changed");
  EXPECT_EQ(events.entries[0].severity, ge::Severity::kWarning);
  ASSERT_TRUE(events.entries[0].detail.is_object()) << "detail must arrive parsed, not as a string";
  EXPECT_EQ(events.entries[0].detail.GetInteger("first_key_seq"), 7);
  EXPECT_EQ(rec.count, 0) << "in scope, the session-services path must not also fire";

  // (b) The scope is gone: the pre-change side-band path, exactly once.
  const ge_status st = ge::HostApiAdapter::vtable().event_publish(&ev);
  EXPECT_EQ(st.code, GE_STATUS_OK);
  EXPECT_EQ(rec.count, 1) << "outside a scope the session services stay the only path";
  EXPECT_EQ(rec.type, "media_format_changed");
  EXPECT_EQ(events.entries.size(), 1U) << "the recording sink sees nothing outside a scope";
}

// A null / empty detail_json is legal (engine.cpp only parses a non-null
// detail): the sink receives an empty JSON object, not a crash and not a
// string.
TEST(PluginEventTest, NullDetailArrivesAsAnEmptyObject) {
  HostRecorder rec;
  ge::HostApiAdapter adapter(/*session=*/78, /*node=*/6, "announce", /*is_source=*/true,
                             RecordingServices(&rec));
  RecordingEventSink events;
  NullEmitSink emits;
  const ge_event ev = FormatEvent(78, 6, /*detail_json=*/nullptr);

  ge::HostApiAdapter::InvokeScope scope(adapter, emits, &events);
  const ge_status st = ge::HostApiAdapter::vtable().event_publish(&ev);
  EXPECT_EQ(st.code, GE_STATUS_OK);
  ASSERT_EQ(events.entries.size(), 1U);
  ASSERT_TRUE(events.entries[0].detail.is_object());
  EXPECT_TRUE(events.entries[0].detail.as_object().empty());
  EXPECT_EQ(rec.count, 0);
}

// The defaulted third parameter is the compatibility contract for a scope
// opened without a scheduler behind it: publish behaves exactly like the
// outside-scope path (side-band only), it does not crash on a null sink.
TEST(PluginEventTest, ScopeWithoutAnEventSinkStaysSideBandOnly) {
  HostRecorder rec;
  ge::HostApiAdapter adapter(/*session=*/79, /*node=*/7, "announce", /*is_source=*/true,
                             RecordingServices(&rec));
  NullEmitSink emits;
  const ge_event ev = FormatEvent(79, 7, R"({"first_key_seq":7})");

  ge::HostApiAdapter::InvokeScope scope(adapter, emits);  // two-arg form: events == nullptr
  const ge_status st = ge::HostApiAdapter::vtable().event_publish(&ev);
  EXPECT_EQ(st.code, GE_STATUS_OK);
  EXPECT_EQ(rec.count, 1) << "no event sink on the scope: the session-services path stays";
  EXPECT_EQ(rec.detail, R"({"first_key_seq":7})");
}

// Pre-flight ruling follow-up: unifying the entry points means attribution
// of an in-scope publish is supplied by the callee (the scheduler stamps the
// node whose Process call is on the stack), not by the ge_event's own
// session_id/source_node_id. A plugin that lies about both -- naming another
// *registered* session -- still gets its side-band copy attributed to the
// calling node, and the spoofed session's services never fire. (Outside a
// scope the caller's fields are honoured; Task 4's
// PluginPublishOutsideAScopeStaysSideBandOnly pins that half.)
class ScopedSpoofingPublisher final : public ge::Operator {
 public:
  ScopedSpoofingPublisher(ge::SessionId session, ge::NodeId node, std::string external_id)
      : adapter_(session, node, std::move(external_id), /*is_source=*/true, ge::HostServices{}) {}
  ge::Status Open(const ge::OpenRequest&) override { return ge::Status::Ok(); }
  ge::Result<ge::ProcessResult> Process(const ge::ProcessRequest& req) override {
    if (req.flags & GE_PROCESS_FLAG_FLUSH) return ge::ProcessResult::kContinue;
    if (req.sink == nullptr) return ge::Status::Internal("no emit sink");
    // Exactly what PluginOperator::Process does around the vtable call.
    ge::HostApiAdapter::InvokeScope scope(adapter_, *req.sink, req.events);
    const ge_event ev = FormatEvent(/*session=*/999, /*node=*/424242, R"({"first_key_seq":7})");
    publish_code.store(ge::HostApiAdapter::vtable().event_publish(&ev).code);
    return ge::ProcessResult::kExhausted;
  }
  ge::Status Close(const ge::CloseRequest&) override { return ge::Status::Ok(); }
  std::atomic<int32_t> publish_code{-1};

 private:
  ge::HostApiAdapter adapter_;
};

TEST(PluginEventTest, InScopePublishIsAttributedToTheCallingNodeNotTheSpoofedIds) {
  // The spoofed session (999) is *registered*: if EventPublish ignored the
  // scope and routed by the struct's session_id, this recorder would fire.
  HostRecorder spoofed;
  ge::HostApiAdapter spoof_target(/*session=*/999, /*node=*/1, "spoof-target",
                                  /*is_source=*/false, RecordingServices(&spoofed));

  struct Observed {
    std::mutex mutex;
    std::vector<std::pair<std::string, ge::JsonValue>> entries;  // node external_id, detail
  } observed;
  ge::BuiltinOperatorFactory factory;
  std::vector<std::shared_ptr<ge::Operator>> keep;
  ScopedSpoofingPublisher* publisher = nullptr;
  factory.Register(Desc("PlugSpoof@1.0.0", {}, {BytesPort("out", ge::PortDirection::kOutput)},
                        true, 1),
                   [&keep, &publisher](const ge::OperatorCreateArgs& args) {
                     auto p = std::make_shared<ScopedSpoofingPublisher>(
                         args.session_id, args.node_id, std::string(args.external_id));
                     publisher = p.get();
                     return Keep(keep, std::move(p));
                   });
  factory.Register(Desc("PlugCollect@1.0.0", {BytesPort("in", ge::PortDirection::kInput)}, {}),
                   [&keep](const ge::OperatorCreateArgs&) {
                     return Keep(keep, std::make_shared<Collector>());
                   });
  ge::GraphBuilder b("plugin-event-attribution");
  auto src = b.AddNode(Op("PlugSpoof@1.0.0"), "announce");
  auto snk = b.AddNode(Op("PlugCollect@1.0.0"), "collect");
  b.Connect(src.port("out"), snk.port("in"), {.id = "e0"});
  auto r = ge::RuntimeTopology::Build(*b.Build(), factory, {.session_id = 42, .version = 1});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  auto topo = *r;
  ge::ExecutorPool exec(0);
  ge::SchedulerEvents events;
  events.on_operator_event = [&observed](ge::NodeRuntime& node, std::string type, ge::Severity,
                                         ge::JsonValue detail) {
    std::lock_guard lock(observed.mutex);
    if (type == "media_format_changed") observed.entries.emplace_back(node.external_id(), detail);
  };
  ge::Scheduler s(topo, exec, std::move(events));
  ASSERT_TRUE(s.OpenAll().ok());
  s.Start();
  for (int i = 0; i < 10000 && !s.all_closed(); ++i) {
    (void)exec.RunOne();
    s.Tick();
  }
  ASSERT_TRUE(s.all_closed());
  ASSERT_NE(publisher, nullptr);
  EXPECT_EQ(publisher->publish_code.load(), GE_STATUS_OK);
  std::lock_guard lock(observed.mutex);
  ASSERT_EQ(observed.entries.size(), 1U) << "one side-band copy through the scheduler's sink";
  EXPECT_EQ(observed.entries[0].first, "announce")
      << "attribution comes from the calling node, not the ge_event's ids";
  EXPECT_EQ(observed.entries[0].second.GetInteger("first_key_seq"), 7);
  EXPECT_EQ(spoofed.count, 0) << "the spoofed session's services must never fire in scope";
}

// Review fix round (mutant 5): the one line that wires the whole feature up
// for real .so plugins is PluginOperator::Process constructing its
// InvokeScope with request.events. Everything above drives HostApiAdapter
// directly, so a revert of that call site to the two-argument form survived
// every test. This one pins it through the real chain -- a hand-built
// ge_operator_vtable (PluginOperator::Create takes it directly; no .so, no
// dlopen) whose process callback publishes through the host api it was
// handed at create. The fake plugin publishes with the *registered* session
// id, so under the two-argument mutant the publish still succeeds -- onto
// the session-services recorder instead of the request's EventSink -- and
// the test fails on routing, not on an incidental NotFound.
struct FakePlugin {
  const ge_host_api* api = nullptr;
  void* host_context = nullptr;
  ge_status last_publish{};
  int process_calls = 0;
};

ge_status FakeCreate(const ge_operator_create_args* args, ge_operator_handle* out) {
  auto* p = new FakePlugin;
  p->api = args->host_api;
  p->host_context = args->host_context;
  *out = reinterpret_cast<ge_operator_handle>(p);
  return ge::OkStatus();
}
ge_status FakeOpen(ge_operator_handle, const ge_open_request*) { return ge::OkStatus(); }
ge_status FakeProcess(ge_operator_handle op, const ge_process_request* req) {
  auto* p = reinterpret_cast<FakePlugin*>(op);
  ++p->process_calls;
  const ge_event ev = FormatEvent(req->session_id, /*node=*/0, R"({"first_key_seq":7})");
  p->last_publish = p->api->event_publish(&ev);
  return ge::OkStatus();
}
ge_status FakeClose(ge_operator_handle, const ge_close_request*) { return ge::OkStatus(); }
void FakeDestroy(ge_operator_handle op) { delete reinterpret_cast<FakePlugin*>(op); }

TEST(PluginEventTest, RealPluginProcessRoutesPublishToTheRequestEventSink) {
  ge_operator_vtable vt{};
  vt.header.struct_size = sizeof(ge_operator_vtable);
  vt.header.abi_major = GE_ABI_MAJOR;
  vt.create = &FakeCreate;
  vt.open = &FakeOpen;
  vt.process = &FakeProcess;
  vt.close = &FakeClose;
  vt.destroy = &FakeDestroy;  // submit stays null: not a mandatory entry

  HostRecorder rec;
  ge::OperatorCreateArgs args;
  args.key = Op("FakePlug@1.0.0");
  args.node_id = 9;
  args.external_id = "announce";
  args.session_id = 88;
  auto created = ge::PluginOperator::Create(vt, args, /*is_source=*/true,
                                            RecordingServices(&rec), /*lease=*/nullptr);
  ASSERT_TRUE(created.ok()) << created.status().ToString();
  std::unique_ptr<ge::PluginOperator> op = std::move(*created);

  ge::OpenRequest open;
  open.session_id = 88;
  ASSERT_TRUE(op->Open(open).ok());

  RecordingEventSink events;
  NullEmitSink emits;
  ge::ProcessRequest req;
  req.session_id = 88;
  req.sink = &emits;
  req.events = &events;  // what the scheduler sets on both source and process paths
  const auto result = op->Process(req);
  ASSERT_TRUE(result.ok()) << result.status().ToString();

  const auto* plugin = reinterpret_cast<const FakePlugin*>(op->handle());
  ASSERT_EQ(plugin->process_calls, 1);
  EXPECT_EQ(plugin->last_publish.code, GE_STATUS_OK);
  ASSERT_EQ(events.entries.size(), 1U)
      << "Process must hand request.events to its InvokeScope; a two-argument "
         "scope silently downgrades every real plugin to side-band only";
  EXPECT_EQ(events.entries[0].type, "media_format_changed");
  EXPECT_EQ(events.entries[0].severity, ge::Severity::kWarning);
  EXPECT_EQ(events.entries[0].detail.GetInteger("first_key_seq"), 7);
  EXPECT_EQ(rec.count, 0) << "in scope, the session-services path must not fire";

  ge::CloseRequest close;
  close.session_id = 88;
  EXPECT_TRUE(op->Close(close).ok());
}

}  // namespace
