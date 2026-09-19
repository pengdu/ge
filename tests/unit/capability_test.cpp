#include <ge/cpp/capability.h>

#include <gtest/gtest.h>

namespace {

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

constexpr const char* kDecoderJson = R"({
  "kind": "CapabilityDescriptor",
  "schema_version": 1,
  "$id": "ge.dev/schema/capability/v1",
  "operator": "VideoDecoder@2.0.0",
  "description": "hw decoder",
  "ports": {
    "inputs": [
      {"name": "packet", "type_tag": "EncodedPacket", "sync": ["any"]}
    ],
    "outputs": [
      {"name": "video", "type_tag": "VideoFrame", "dynamic_consumers": true,
       "video": {"pixel_formats": ["NV12", "P010", "YUV420P"],
                 "width": {"min": 64, "max": 4096}, "height": {"min": 64, "max": 4096},
                 "fps": {"min": 1, "max": 120},
                 "color_spaces": ["bt709", "bt2020"]},
       "memory": {"kinds": ["cuda_device", "host"], "device_ids": [0, 1]}}
    ]
  },
  "execution": {"devices": ["gpu", "cpu"], "stateful": true, "async": false,
                "max_parallelism": 1, "zero_copy": true},
  "parameters": {"hot_updatable": ["skip_frames"], "migratable": ["skip_frames"],
                 "schema": {"skip_frames": {"type": "integer"}}},
  "resources": {"gpu_memory_bytes": 268435456},
  "events": {"emits": ["MediaFormatChanged"], "accepts": []}
})";

ge::PortCapability VideoOut(std::vector<std::string> pf) {
  ge::PortCapability p;
  p.name = "out";
  p.direction = ge::PortDirection::kOutput;
  p.type_tag = "VideoFrame";
  p.video = ge::VideoConstraints{};
  p.video->pixel_formats = std::move(pf);
  p.video->width = ge::IntRange{64, 4096};
  p.video->height = ge::IntRange{64, 4096};
  return p;
}

ge::PortCapability VideoIn(std::vector<std::string> pf, const char* name = "in") {
  ge::PortCapability p;
  p.name = name;
  p.direction = ge::PortDirection::kInput;
  p.type_tag = "VideoFrame";
  p.video = ge::VideoConstraints{};
  p.video->pixel_formats = std::move(pf);
  p.video->width = ge::IntRange{320, 1920};
  return p;
}

TEST(CapabilityDescriptorTest, ParsesFullDocument) {
  const auto r = ge::CapabilityDescriptor::ParseJson(kDecoderJson);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  const ge::CapabilityDescriptor& d = *r;
  EXPECT_EQ(d.op.ToString(), "VideoDecoder@2.0.0");
  EXPECT_EQ(d.description, "hw decoder");
  ASSERT_EQ(d.inputs.size(), 1U);
  ASSERT_EQ(d.outputs.size(), 1U);
  const ge::PortCapability* in = d.FindInput("packet");
  ASSERT_NE(in, nullptr);
  EXPECT_EQ(in->direction, ge::PortDirection::kInput);
  ASSERT_TRUE(in->sync.has_value());
  EXPECT_EQ(in->sync->at(0), ge::SyncPolicy::kAny);
  const ge::PortCapability* out = d.FindOutput("video");
  ASSERT_NE(out, nullptr);
  EXPECT_TRUE(out->dynamic_consumers);
  ASSERT_TRUE(out->video.has_value());
  EXPECT_EQ(out->video->pixel_formats->size(), 3U);
  EXPECT_EQ(out->video->width, (ge::IntRange{64, 4096}));
  EXPECT_EQ(out->video->fps, (ge::RationalRange{1, 120}));
  ASSERT_TRUE(out->memory.kinds.has_value());
  EXPECT_EQ(out->memory.kinds->at(0), ge::MemoryKind::kCudaDevice);
  EXPECT_EQ(out->memory.device_ids->size(), 2U);
  EXPECT_EQ(d.execution.devices.at(0), ge::DeviceKind::kGpu);
  EXPECT_TRUE(d.execution.stateful);
  EXPECT_TRUE(d.execution.zero_copy);
  EXPECT_EQ(d.parameters.hot_updatable.at(0), "skip_frames");
  EXPECT_EQ(d.resources.amounts.at("gpu_memory_bytes"), 268435456);
  EXPECT_EQ(d.events.emits.at(0), "MediaFormatChanged");
  EXPECT_EQ(d.FindInput("nope"), nullptr);
}

TEST(CapabilityDescriptorTest, RoundTripAndVersionStable) {
  const auto r = ge::CapabilityDescriptor::ParseJson(kDecoderJson);
  ASSERT_TRUE(r.ok());
  const std::string once = r->Serialize();
  const auto again = ge::CapabilityDescriptor::ParseJson(once);
  ASSERT_TRUE(again.ok()) << again.status().ToString();
  EXPECT_EQ(*r, *again);
  EXPECT_EQ(once, again->Serialize());
  EXPECT_EQ(r->Version(), again->Version());
  ge::CapabilityDescriptor changed = *r;
  changed.outputs[0].video->pixel_formats->push_back("RGBA");
  EXPECT_NE(changed.Version(), r->Version());
}

TEST(CapabilityDescriptorTest, RejectsInvalid) {
  const auto code = [](const char* body) {
    std::string doc = R"({"kind":"CapabilityDescriptor","schema_version":1,)";
    doc += body;
    doc += "}";
    return ge::CapabilityDescriptor::ParseJson(doc).status().code();
  };
  EXPECT_EQ(code(R"("operator":"X@1.0.0")"), GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("operator":"X@1.0.0","ports":{},"execution":{"stateful":true,"max_parallelism":4})"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("operator":"X@1.0.0","ports":{},"execution":{"async":true})"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("operator":"X@1.0.0","ports":{"inputs":[{"name":"a","type_tag":"T"},{"name":"a","type_tag":"T"}]})"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("operator":"X@1.0.0","ports":{"outputs":[{"name":"a","type_tag":"T","memory":{"kinds":["mars"]}}]})"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("operator":"X@1.0.0","ports":{},"typo":1)"), GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(ge::CapabilityDescriptor::ParseJson(R"({"kind":"GraphSpec","schema_version":1})").status().code(),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(ge::CapabilityDescriptor::ParseJson("not json").status().code(),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
}

TEST(NegotiatorTest, EdgeSelectsByOutputPriorityThenIntersectsRanges) {
  ge::CapabilityNegotiator n;
  const auto out = VideoOut({"P010", "NV12"});
  const auto in = VideoIn({"NV12", "P010"});
  const auto r = n.NegotiateEdge(Op("A@1.0.0"), 11, out, Op("B@1.0.0"), 22, in, std::nullopt, {});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->logical_type, "VideoFrame");
  ASSERT_TRUE(r->video.has_value());
  EXPECT_EQ(r->video->pixel_format, "NV12");  // input priority wins over output
  EXPECT_EQ(r->video->width, (ge::IntRange{320, 1920}));
  EXPECT_EQ(r->video->height, (ge::IntRange{64, 4096}));
  EXPECT_EQ(r->memory_kind, ge::MemoryKind::kHost);
  EXPECT_EQ(r->device_id, -1);
  EXPECT_EQ(r->sync_policy, ge::SyncPolicy::kAny);
  EXPECT_EQ(r->source_capability, 11U);
  EXPECT_EQ(r->target_capability, 22U);
  EXPECT_EQ(n.cache_size(), 1U);
}

TEST(NegotiatorTest, CallerPreferenceOverridesPortPriority) {
  ge::CapabilityNegotiator n;
  ge::PreferenceSet pref;
  pref.ordered["pixel_format"] = {"P010"};
  const auto r = n.NegotiateEdge(Op("A@1.0.0"), 1, VideoOut({"NV12", "P010"}), Op("B@1.0.0"), 1,
                                 VideoIn({"NV12", "P010"}), std::nullopt, pref);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->video->pixel_format, "P010");
}

TEST(NegotiatorTest, ConflictReportsDimensionAndConverter) {
  ge::CapabilityNegotiator n;
  const auto r = n.NegotiateEdge(Op("A@1.0.0"), 1, VideoOut({"NV12"}), Op("B@1.0.0"), 1,
                                 VideoIn({"RGBA"}), std::nullopt, {});
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), GE_STATUS_CAPABILITY_CONFLICT);
  ASSERT_TRUE(n.last_conflict().has_value());
  EXPECT_EQ(n.last_conflict()->dimension, "pixel_format");
  EXPECT_EQ(n.last_conflict()->suggested_converter, "VideoConvert");
  EXPECT_NE(r.status().context_json().find("NV12"), std::string::npos);

  auto out = VideoOut({"NV12"});
  out.video->width = ge::IntRange{4000, 4096};
  const auto r2 = n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("B@1.0.0"), 1, VideoIn({"NV12"}),
                                  std::nullopt, {});
  ASSERT_FALSE(r2.ok());
  EXPECT_EQ(n.last_conflict()->dimension, "width");
  EXPECT_EQ(n.last_conflict()->suggested_converter, "VideoScale");
}

TEST(NegotiatorTest, OpaqueTagsCompareExactlyOnly) {
  ge::CapabilityNegotiator n;
  ge::PortCapability out;
  out.name = "o";
  out.direction = ge::PortDirection::kOutput;
  out.type_tag = "com.acme.Detections";
  out.video = ge::VideoConstraints{};
  out.video->pixel_formats = {"NV12"};
  ge::PortCapability in = out;
  in.name = "i";
  in.direction = ge::PortDirection::kInput;
  in.video->pixel_formats = {"RGBA"};  // ignored for opaque tags
  const auto ok = n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("B@1.0.0"), 1, in, std::nullopt, {});
  ASSERT_TRUE(ok.ok()) << ok.status().ToString();
  EXPECT_FALSE(ok->video.has_value());
  in.type_tag = "com.acme.Other";
  const auto bad = n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("B@1.0.0"), 2, in, std::nullopt, {});
  EXPECT_EQ(bad.status().code(), GE_STATUS_CAPABILITY_CONFLICT);
  EXPECT_EQ(n.last_conflict()->dimension, "logical_type");
}

TEST(NegotiatorTest, MemoryAndSyncDimensions) {
  ge::CapabilityNegotiator n;
  auto out = VideoOut({"NV12"});
  out.memory.kinds = {ge::MemoryKind::kCudaDevice, ge::MemoryKind::kHost};
  out.memory.device_ids = {1, 0};
  auto in = VideoIn({"NV12"});
  in.memory.kinds = {ge::MemoryKind::kCudaDevice};
  in.sync = {ge::SyncPolicy::kAligned, ge::SyncPolicy::kLatest};
  const auto r = n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("B@1.0.0"), 1, in, std::nullopt, {});
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->memory_kind, ge::MemoryKind::kCudaDevice);
  EXPECT_EQ(r->device_id, 0);
  EXPECT_EQ(r->sync_policy, ge::SyncPolicy::kAligned);

  const auto r2 = n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("B@1.0.0"), 1, in, ge::SyncPolicy::kLatest, {});
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(r2->sync_policy, ge::SyncPolicy::kLatest);

  const auto r3 = n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("B@1.0.0"), 1, in, ge::SyncPolicy::kAny, {});
  EXPECT_EQ(r3.status().code(), GE_STATUS_CAPABILITY_CONFLICT);
  EXPECT_EQ(n.last_conflict()->dimension, "sync_policy");

  in.memory.kinds = {ge::MemoryKind::kDmaBuf};
  const auto r4 = n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("B@1.0.0"), 2, in, std::nullopt, {});
  EXPECT_EQ(r4.status().code(), GE_STATUS_CAPABILITY_CONFLICT);
  EXPECT_EQ(n.last_conflict()->dimension, "memory_kind");
  EXPECT_EQ(n.last_conflict()->suggested_converter, "MemoryCopy");
}

TEST(NegotiatorTest, AudioAndTensor) {
  ge::CapabilityNegotiator n;
  ge::PortCapability out;
  out.name = "o";
  out.direction = ge::PortDirection::kOutput;
  out.type_tag = "AudioFrame";
  out.audio = ge::AudioConstraints{};
  out.audio->sample_formats = {"fltp", "s16"};
  out.audio->sample_rate = ge::IntRange{8000, 96000};
  ge::PortCapability in;
  in.name = "i";
  in.direction = ge::PortDirection::kInput;
  in.type_tag = "AudioFrame";
  in.audio = ge::AudioConstraints{};
  in.audio->sample_formats = {"s16"};
  in.audio->sample_rate = ge::IntRange{44100, 48000};
  in.audio->channel_layouts = {"stereo"};
  const auto a = n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("B@1.0.0"), 1, in, std::nullopt, {});
  ASSERT_TRUE(a.ok()) << a.status().ToString();
  EXPECT_EQ(a->audio->sample_format, "s16");
  EXPECT_EQ(a->audio->sample_rate, (ge::IntRange{44100, 48000}));
  EXPECT_EQ(a->audio->channel_layout, "stereo");

  ge::PortCapability tout;
  tout.name = "o";
  tout.direction = ge::PortDirection::kOutput;
  tout.type_tag = "Tensor";
  tout.tensor = ge::TensorConstraints{};
  tout.tensor->dtypes = {"float32", "float16"};
  tout.tensor->shape = {{1, 1}, {3, 3}, {0, -1}, {0, -1}};
  ge::PortCapability tin = tout;
  tin.name = "i";
  tin.direction = ge::PortDirection::kInput;
  tin.tensor->dtypes = {"float16"};
  tin.tensor->layouts = {"NCHW"};
  tin.tensor->shape = {{1, 1}, {3, 3}, {224, 224}, {224, 224}};
  const auto t = n.NegotiateEdge(Op("T@1.0.0"), 1, tout, Op("U@1.0.0"), 1, tin, std::nullopt, {});
  ASSERT_TRUE(t.ok()) << t.status().ToString();
  EXPECT_EQ(t->tensor->dtype, "float16");
  EXPECT_EQ(t->tensor->layout, "NCHW");
  ASSERT_EQ(t->tensor->shape.size(), 4U);
  EXPECT_EQ(t->tensor->shape[2], (ge::IntRange{224, 224}));

  tin.tensor->shape = {{1, 1}, {3, 3}};
  const auto rank = n.NegotiateEdge(Op("T@1.0.0"), 1, tout, Op("U@1.0.0"), 2, tin, std::nullopt, {});
  EXPECT_EQ(rank.status().code(), GE_STATUS_CAPABILITY_CONFLICT);
  EXPECT_EQ(n.last_conflict()->dimension, "shape_rank");
}

TEST(NegotiatorTest, FanoutCommonContractIsOrderIndependent) {
  ge::CapabilityNegotiator n;
  const auto out = VideoOut({"P010", "NV12", "YUV420P"});
  const auto a = VideoIn({"YUV420P", "NV12"}, "in");
  const auto b = VideoIn({"NV12", "P010"}, "in");
  ge::FanoutConsumer ca{.node_id = "enc_a", .op = Op("Enc@1.0.0"), .capability = 5, .port = &a};
  ge::FanoutConsumer cb{.node_id = "enc_b", .op = Op("Enc@1.0.0"), .capability = 6, .port = &b};
  const auto r1 = n.NegotiateFanout(Op("Dec@1.0.0"), 1, out, {ca, cb}, {});
  const auto r2 = n.NegotiateFanout(Op("Dec@1.0.0"), 1, out, {cb, ca}, {});
  ASSERT_TRUE(r1.ok()) << r1.status().ToString();
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(*r1, *r2);
  EXPECT_EQ(r1->video->pixel_format, "NV12");

  const auto c = VideoIn({"RGBA"}, "in");
  ge::FanoutConsumer cc{.node_id = "enc_c", .op = Op("Enc@1.0.0"), .capability = 7, .port = &c};
  const auto bad = n.NegotiateFanout(Op("Dec@1.0.0"), 1, out, {ca, cb, cc}, {});
  EXPECT_EQ(bad.status().code(), GE_STATUS_CAPABILITY_CONFLICT);
  ASSERT_EQ(n.last_conflict()->consumers.size(), 3U);
  EXPECT_EQ(n.last_conflict()->consumers[0], "enc_a.in");
}

TEST(NegotiatorTest, FanoutAddConsumerMustKeepExistingContract) {
  ge::CapabilityNegotiator n;
  const auto out = VideoOut({"NV12", "P010"});
  const auto a = VideoIn({"NV12", "P010"}, "in");
  ge::FanoutConsumer ca{.node_id = "a", .op = Op("Enc@1.0.0"), .capability = 1, .port = &a};
  const auto base = n.NegotiateFanout(Op("Dec@1.0.0"), 1, out, {ca}, {});
  ASSERT_TRUE(base.ok());

  const auto compatible = VideoIn({"NV12"}, "in");
  ge::FanoutConsumer cb{.node_id = "b", .op = Op("Enc@1.0.0"), .capability = 2, .port = &compatible};
  EXPECT_TRUE(n.NegotiateFanout(Op("Dec@1.0.0"), 1, out, {ca, cb}, {}, &*base).ok());

  const auto narrowing = VideoIn({"P010"}, "in");
  ge::FanoutConsumer cn{.node_id = "c", .op = Op("Enc@1.0.0"), .capability = 3, .port = &narrowing};
  const auto r = n.NegotiateFanout(Op("Dec@1.0.0"), 1, out, {ca, cn}, {}, &*base);
  EXPECT_EQ(r.status().code(), GE_STATUS_CAPABILITY_CONFLICT);
  EXPECT_EQ(n.last_conflict()->dimension, "existing_contract");
}

TEST(NegotiatorTest, CacheKeyIgnoresPortContentUntilVersionBumps) {
  ge::CapabilityNegotiator n;
  const auto out = VideoOut({"NV12"});
  auto in = VideoIn({"NV12"});
  ASSERT_TRUE(n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("B@1.0.0"), 1, in, std::nullopt, {}).ok());
  in.video->pixel_formats = {"RGBA"};
  EXPECT_TRUE(n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("B@1.0.0"), 1, in, std::nullopt, {}).ok());
  EXPECT_EQ(n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("B@1.0.0"), 2, in, std::nullopt, {}).status().code(),
            GE_STATUS_CAPABILITY_CONFLICT);
  EXPECT_EQ(n.cache_size(), 1U);
}

TEST(NegotiatorTest, CacheInvalidation) {
  ge::CapabilityNegotiator n;
  const auto out = VideoOut({"NV12"});
  const auto in = VideoIn({"NV12"});
  ASSERT_TRUE(n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("B@1.0.0"), 1, in, std::nullopt, {}).ok());
  ASSERT_TRUE(n.NegotiateEdge(Op("A@1.0.0"), 1, out, Op("C@1.0.0"), 1, in, std::nullopt, {}).ok());
  ASSERT_TRUE(n.NegotiateEdge(Op("A@1.0.0"), 2, out, Op("B@1.0.0"), 1, in, std::nullopt, {}).ok());
  EXPECT_EQ(n.cache_size(), 3U);
  n.InvalidateOperator(Op("C@1.0.0"));
  EXPECT_EQ(n.cache_size(), 2U);
  n.InvalidateOperator(Op("A@1.0.0"));
  EXPECT_EQ(n.cache_size(), 0U);
}

}  // namespace
