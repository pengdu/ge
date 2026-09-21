#include <ge/cpp/resource_ledger.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using ge::ResourceAmount;
using ge::ResourceCapacity;
using ge::ResourceKind;
using ge::ResourceLease;
using ge::ResourceLedger;

ResourceAmount Cpu(std::uint64_t n) { return {ResourceKind::kCpuThreads, -1, n}; }
ResourceAmount Mem(std::uint64_t n) { return {ResourceKind::kHostMemory, -1, n}; }
ResourceAmount Gpu(std::int32_t dev, std::uint64_t n) { return {ResourceKind::kGpuMemory, dev, n}; }

TEST(ResourceLedgerTest, ReserveIsAllOrNothing) {
  ResourceLedger ledger({{ResourceKind::kCpuThreads, -1, 4}, {ResourceKind::kHostMemory, -1, 1000}});
  auto a = ledger.Reserve(1, {Cpu(3), Mem(500)});
  ASSERT_TRUE(a.ok()) << a.status().ToString();
  EXPECT_EQ(ledger.UsageOf(ResourceKind::kCpuThreads, -1).reserved, 3U);
  EXPECT_EQ(ledger.UsageOf(ResourceKind::kHostMemory, -1).reserved, 500U);

  // cpu fits (1 left), memory does not: nothing may be taken.
  auto b = ledger.Reserve(2, {Cpu(1), Mem(600)});
  ASSERT_FALSE(b.ok());
  EXPECT_EQ(b.status().code(), GE_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_TRUE(b.status().retryable());
  EXPECT_EQ(ledger.UsageOf(ResourceKind::kCpuThreads, -1).reserved, 3U);
  EXPECT_EQ(ledger.UsageOf(ResourceKind::kHostMemory, -1).reserved, 500U);
  EXPECT_EQ(ledger.live_leases(), 1U);

  // RES-3: the error names every short dimension with the four numbers.
  const auto ctx = ge::ParseJson(b.status().context_json());
  ASSERT_TRUE(ctx.ok());
  const auto& shorts = ctx.value->as_object().at("short").as_array();
  ASSERT_EQ(shorts.size(), 1U);
  const auto& s0 = shorts[0].as_object();
  EXPECT_EQ(s0.at("kind").as_string(), "host_memory");
  EXPECT_EQ(s0.at("requested").as_integer(), 600);
  EXPECT_EQ(s0.at("reserved").as_integer(), 500);
  EXPECT_EQ(s0.at("capacity").as_integer(), 1000);
  EXPECT_EQ(s0.at("available").as_integer(), 500);
  EXPECT_TRUE(ctx.value->as_object().at("retry_after_release").as_bool());
}

TEST(ResourceLedgerTest, LeaseReturnsOnDestroyAndReleaseIsIdempotent) {
  ResourceLedger ledger({{ResourceKind::kCpuThreads, -1, 2}});
  {
    auto a = ledger.Reserve(1, {Cpu(2)});
    ASSERT_TRUE(a.ok());
    EXPECT_FALSE(ledger.Reserve(2, {Cpu(1)}).ok());
    a->Release();
    a->Release();
    EXPECT_EQ(ledger.UsageOf(ResourceKind::kCpuThreads, -1).reserved, 0U);
    EXPECT_EQ(ledger.live_leases(), 0U);
    EXPECT_FALSE(a->active());
  }
  {
    auto b = ledger.Reserve(3, {Cpu(2)});
    ASSERT_TRUE(b.ok());
    ResourceLease moved = std::move(*b);
    EXPECT_TRUE(moved.active());
    EXPECT_FALSE(b->active());
  }  // moved dies here
  EXPECT_EQ(ledger.UsageOf(ResourceKind::kCpuThreads, -1).reserved, 0U);
  EXPECT_EQ(ledger.UsageOf(ResourceKind::kCpuThreads, -1).peak, 2U);
}

TEST(ResourceLedgerTest, UnlistedKindsAreUnlimitedButTracked) {
  ResourceLedger ledger({{ResourceKind::kCpuThreads, -1, 1}});
  auto a = ledger.Reserve(1, {Cpu(1), Gpu(0, 1ULL << 40)});
  ASSERT_TRUE(a.ok());
  const auto u = ledger.UsageOf(ResourceKind::kGpuMemory, 0);
  EXPECT_EQ(u.capacity, 0U);
  EXPECT_EQ(u.reserved, 1ULL << 40);
  EXPECT_FALSE(ledger.Capacity(ResourceKind::kGpuMemory, 0).has_value());
}

TEST(ResourceLedgerTest, PerDeviceAccountsAreIndependent) {
  ResourceLedger ledger({{ResourceKind::kGpuMemory, 0, 100}, {ResourceKind::kGpuMemory, 1, 100}});
  auto a = ledger.Reserve(1, {Gpu(0, 100)});
  ASSERT_TRUE(a.ok());
  EXPECT_FALSE(ledger.Reserve(2, {Gpu(0, 1)}).ok());
  EXPECT_TRUE(ledger.Reserve(2, {Gpu(1, 100)}).ok());
}

TEST(ResourceLedgerTest, MergeSumsDuplicatesAndDropsZeros) {
  auto m = ge::MergeAmounts({Cpu(1), Mem(0), Cpu(2), Gpu(0, 5), Gpu(0, 5)});
  ASSERT_EQ(m.size(), 2U);
  EXPECT_EQ(m[0].kind, ResourceKind::kCpuThreads);
  EXPECT_EQ(m[0].amount, 3U);
  EXPECT_EQ(m[1].kind, ResourceKind::kGpuMemory);
  EXPECT_EQ(m[1].amount, 10U);
}

TEST(ResourceLedgerTest, CommitGrowsShrinksAndRollsBackOnShortfall) {
  ResourceLedger ledger({{ResourceKind::kHostMemory, -1, 1000}});
  auto a = ledger.Reserve(1, {Mem(300)});
  ASSERT_TRUE(a.ok());
  auto b = ledger.Reserve(2, {Mem(300)});
  ASSERT_TRUE(b.ok());
  // Grow within capacity.
  ASSERT_TRUE(a->Commit({Mem(600)}).ok());
  EXPECT_EQ(ledger.UsageOf(ResourceKind::kHostMemory, -1).reserved, 900U);
  EXPECT_EQ(a->amounts()[0].amount, 600U);
  // Grow past capacity: lease keeps its previous amount.
  auto s = a->Commit({Mem(800)});
  EXPECT_EQ(s.code(), GE_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_EQ(a->amounts()[0].amount, 600U);
  EXPECT_EQ(ledger.UsageOf(ResourceKind::kHostMemory, -1).reserved, 900U);
  // Shrink and drop a dimension.
  ASSERT_TRUE(a->Commit({Mem(100)}).ok());
  EXPECT_EQ(ledger.UsageOf(ResourceKind::kHostMemory, -1).reserved, 400U);
  ASSERT_TRUE(a->Commit({}).ok());
  EXPECT_EQ(ledger.UsageOf(ResourceKind::kHostMemory, -1).reserved, 300U);
  EXPECT_TRUE(a->amounts().empty());
}

TEST(ResourceLedgerTest, ParseKindAcceptsCapabilitySpellings) {
  EXPECT_EQ(ge::ParseResourceKind("gpu_memory_bytes"), ResourceKind::kGpuMemory);
  EXPECT_EQ(ge::ParseResourceKind("gpu_memory"), ResourceKind::kGpuMemory);
  EXPECT_EQ(ge::ParseResourceKind("host_memory_bytes"), ResourceKind::kHostMemory);
  EXPECT_EQ(ge::ParseResourceKind("edge_buffer_bytes"), ResourceKind::kEdgeBufferBytes);
  EXPECT_EQ(ge::ParseResourceKind("cpu_threads"), ResourceKind::kCpuThreads);
  EXPECT_FALSE(ge::ParseResourceKind("unicorns").has_value());
}

TEST(ResourceLedgerTest, NodeEstimateParsesDeviceSuffixAndDefaults) {
  ge::CapabilityDescriptor cap;
  cap.execution.devices = {ge::DeviceKind::kGpu};
  cap.resources.amounts["cpu_threads"] = 2;
  cap.resources.amounts["gpu_memory_bytes"] = 100;   // -> device 0 (first non-cpu)
  cap.resources.amounts["nvenc_sessions@1"] = 1;     // explicit device
  cap.resources.amounts["unicorns"] = 7;             // ignored
  cap.resources.amounts["pinned_memory@3"] = 5;      // host-wide: suffix dropped
  const auto est = ge::EstimateNodeResources(cap);
  ASSERT_EQ(est.size(), 4U);
  EXPECT_EQ(est[0], (ResourceAmount{ResourceKind::kCpuThreads, -1, 2}));
  EXPECT_EQ(est[1], (ResourceAmount{ResourceKind::kPinnedMemory, -1, 5}));
  EXPECT_EQ(est[2], (ResourceAmount{ResourceKind::kGpuMemory, 0, 100}));
  EXPECT_EQ(est[3], (ResourceAmount{ResourceKind::kNvencSessions, 1, 1}));
}

TEST(ResourceLedgerTest, PacketBytesFromContract) {
  ge::ConnectionContract c;
  c.video = ge::SelectedVideoFormat{.pixel_format = "yuv420p", .width = ge::IntRange{1920, 1920},
                                    .height = ge::IntRange{1080, 1080}};
  EXPECT_EQ(ge::EstimatePacketBytes(c), 1920ULL * 1080 * 12 / 8);
  c.video->pixel_format = "nope";
  EXPECT_FALSE(ge::EstimatePacketBytes(c).has_value());
  c.video.reset();
  c.tensor = ge::SelectedTensorFormat{.dtype = "float32", .shape = {ge::IntRange{3, 3}, ge::IntRange{224, 224}, ge::IntRange{224, 224}}};
  EXPECT_EQ(ge::EstimatePacketBytes(c), 3ULL * 224 * 224 * 4);
  c.tensor->shape[0] = ge::IntRange{-1, -1};  // dynamic
  EXPECT_FALSE(ge::EstimatePacketBytes(c).has_value());
  c.tensor.reset();
  c.audio = ge::SelectedAudioFormat{.sample_format = "fltp"};
  EXPECT_FALSE(ge::EstimatePacketBytes(c).has_value());
}

TEST(ResourceLedgerTest, GraphEstimateSumsNodesAndBudgetsEdges) {
  ge::GraphSpec spec("g");
  ASSERT_TRUE(spec.AddNode({.id = "a", .op = *ge::OperatorKey::Parse("A@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "b", .op = *ge::OperatorKey::Parse("B@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddNode({.id = "c", .op = *ge::OperatorKey::Parse("B@1.0.0")}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "e0", .from = {"a", "out"}, .to = {"b", "in"}, .queue = {.capacity = 4}}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "e1", .from = {"b", "out"}, .to = {"c", "in"},
                            .queue = {.capacity = 10, .max_packet_bytes = 50}}).ok());
  ASSERT_TRUE(spec.AddEdge({.id = "e2", .from = {"a", "out"}, .to = {"c", "in"}, .queue = {.capacity = 8}}).ok());
  std::map<std::string, ge::ConnectionContract> contracts;
  ge::ConnectionContract v;
  v.video = ge::SelectedVideoFormat{.pixel_format = "nv12", .width = ge::IntRange{100, 100}, .height = ge::IntRange{100, 100}};
  contracts["e0"] = v;   // 100*100*12/8 = 15000 per packet x4
  contracts["e2"] = ge::ConnectionContract{};  // bytes: unknown
  const auto describe = [](const ge::OperatorKey& k) -> std::optional<ge::CapabilityDescriptor> {
    ge::CapabilityDescriptor d;
    d.op = k;
    d.resources.amounts["cpu_threads"] = k.type_name == "A" ? 2 : 1;
    return d;
  };
  const auto est = ge::EstimateGraphResources(spec, contracts, describe);
  ASSERT_EQ(est.amounts.size(), 2U);
  EXPECT_EQ(est.amounts[0], (ResourceAmount{ResourceKind::kCpuThreads, -1, 4}));
  EXPECT_EQ(est.amounts[1], (ResourceAmount{ResourceKind::kEdgeBufferBytes, -1, 15000ULL * 4 + 50ULL * 10}));
  EXPECT_EQ(est.unbudgeted_edges, std::vector<std::string>{"e2"});
  // Subset: only node b and edge e1.
  const auto sub = ge::EstimateGraphResources(spec, contracts, describe, {"b"}, {"e1"});
  ASSERT_EQ(sub.amounts.size(), 2U);
  EXPECT_EQ(sub.amounts[0].amount, 1U);
  EXPECT_EQ(sub.amounts[1].amount, 500U);
  EXPECT_TRUE(sub.unbudgeted_edges.empty());
}

TEST(ResourceLedgerTest, DefaultCapacitiesCoverCpuAndHostMemory) {
  const auto caps = ResourceLedger::DefaultCapacities(6);
  ASSERT_GE(caps.size(), 1U);
  EXPECT_EQ(caps[0].kind, ResourceKind::kCpuThreads);
  EXPECT_EQ(caps[0].capacity, 6U);
  const auto zero = ResourceLedger::DefaultCapacities(0);
  EXPECT_GE(zero[0].capacity, 1U);
}

}  // namespace
