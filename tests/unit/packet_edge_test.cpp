#include <ge/cpp/edge_channel.h>
#include <ge/cpp/input_binding.h>
#include <ge/cpp/packet.h>

#include <gtest/gtest.h>

#include <thread>

namespace {

ge::PacketRef Data(ge::PacketSeq seq, std::int64_t pts = 0, std::uint32_t flags = 0) {
  auto p = std::make_shared<ge::Packet>();
  p->header.seq = seq;
  p->header.pts_ns = pts;
  p->header.flags = flags;
  p->header.type_tag = ge::TypeTagRegistry::Global().Intern("Bytes");
  return p;
}

TEST(BufferTest, RefcountAndPool) {
  auto pool = ge::HostBufferPool::Create({.max_cached_bytes = 1u << 20});
  ge::BufferRef a = pool->Allocate(100);
  ASSERT_TRUE(a);
  EXPECT_EQ(a->size, 100U);
  EXPECT_GE(a->capacity, 256U);
  EXPECT_EQ(a.use_count(), 1U);
  {
    ge::BufferRef b = a;
    EXPECT_EQ(a.use_count(), 2U);
    ge::BufferRef c = std::move(b);
    EXPECT_EQ(a.use_count(), 2U);
    EXPECT_FALSE(b);
  }
  EXPECT_EQ(a.use_count(), 1U);
  ge::Buffer* raw = a.get();
  a.Reset();
  EXPECT_EQ(pool->stats().live_buffers, 0U);
  EXPECT_GE(pool->stats().cached_bytes, 256U);
  ge::BufferRef again = pool->Allocate(200);
  EXPECT_EQ(again.get(), raw);  // recycled
  EXPECT_EQ(pool->stats().pool_hits, 1U);

  ge::BufferRef big = pool->Allocate(1u << 24);
  ASSERT_TRUE(big);
  std::weak_ptr<ge::HostBufferPool> weak = pool;
  pool.reset();
  EXPECT_FALSE(weak.expired());  // live buffers keep the pool alive
  big.Reset();
  again.Reset();
  EXPECT_TRUE(weak.expired());
}

TEST(BufferTest, ExternalWrapAndHandleRoundTrip) {
  int freed = 0;
  char storage[16];
  ge::BufferRef ext = ge::WrapExternalBuffer(
      storage, sizeof storage, ge::MemoryKind::kHost, -1,
      [](ge::Buffer*, void* ctx) { ++*static_cast<int*>(ctx); }, &freed);
  ge_buffer_handle h = ext.handle();
  ge::BufferRef from_handle = ge::BufferRef::FromHandle(h);
  EXPECT_EQ(ext.use_count(), 2U);
  const ge_buffer_view v = ext->View();
  EXPECT_EQ(v.data, storage);
  EXPECT_EQ(v.size, sizeof storage);
  EXPECT_EQ(v.memory_kind, GE_MEMORY_HOST);
  ext.Reset();
  EXPECT_EQ(freed, 0);
  from_handle.Reset();
  EXPECT_EQ(freed, 1);
}

TEST(PacketTest, TypeTagsAndMetadata) {
  auto& reg = ge::TypeTagRegistry::Global();
  const auto video = reg.Find("VideoFrame");
  EXPECT_NE(video, ge::kInvalidTypeTag);
  EXPECT_TRUE(reg.IsBuiltin(video));
  const auto biz = reg.Intern("biz.UserProfile@v1");
  EXPECT_FALSE(reg.IsBuiltin(biz));
  EXPECT_EQ(reg.Intern("biz.UserProfile@v1"), biz);
  EXPECT_EQ(reg.Name(biz), "biz.UserProfile@v1");
  EXPECT_EQ(reg.Find("nope"), ge::kInvalidTypeTag);

  ge::Metadata m;
  m.Set("zeta", "1");
  m.Set("alpha", "2");
  m.Set("zeta", "3");
  EXPECT_EQ(m.size(), 2U);
  EXPECT_EQ(*m.Get("zeta"), "3");
  EXPECT_EQ(m.entries()[0].first, "alpha");
  const std::string json = m.ToJson();
  const auto back = ge::Metadata::FromJson(json);
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(*back, m);
  EXPECT_FALSE(ge::Metadata::FromJson("[1]").ok());

  ge::Packet p;
  EXPECT_TRUE(p.meta().empty());
  const ge::Packet eos = ge::Packet::Eos(video, 3, 4, 9);
  EXPECT_TRUE(eos.is_eos());
  EXPECT_EQ(eos.header.topology_version, 3U);
  EXPECT_FALSE(eos.payload);
}

ge::EdgeChannel MakeEdge(ge::DropPolicy policy, std::uint32_t cap = 2) {
  ge::EdgeConfig cfg;
  cfg.capacity = cap;
  cfg.policy = policy;
  return ge::EdgeChannel(1, "e", ge::ConnectionContract{}, cfg, {"a", "o"}, {"b", "i"});
}

TEST(EdgeChannelTest, FifoAndMetrics) {
  auto e = MakeEdge(ge::DropPolicy::kBlock, 3);
  EXPECT_EQ(e.Push(Data(1)), ge::PushOutcome::kAccepted);
  EXPECT_EQ(e.Push(Data(2)), ge::PushOutcome::kAccepted);
  EXPECT_EQ(e.depth(), 2U);
  EXPECT_EQ((*e.Pop())->header.seq, 1U);
  EXPECT_EQ((*e.Pop())->header.seq, 2U);
  EXPECT_FALSE(e.Pop().has_value());
  const auto m = e.metrics().Load();
  EXPECT_EQ(m.pushed, 2U);
  EXPECT_EQ(m.popped, 2U);
  EXPECT_EQ(m.max_depth, 2U);
  EXPECT_EQ(m.queue_depth, 0U);
}

TEST(EdgeChannelTest, BlockPolicyMarksBackpressured) {
  auto e = MakeEdge(ge::DropPolicy::kBlock);
  EXPECT_EQ(e.Push(Data(1)), ge::PushOutcome::kAccepted);
  EXPECT_EQ(e.Push(Data(2)), ge::PushOutcome::kAccepted);
  EXPECT_EQ(e.Push(Data(3)), ge::PushOutcome::kWouldBlock);
  EXPECT_EQ(e.state(), ge::EdgeState::kBackpressured);
  EXPECT_FALSE(e.ClearBackpressure());
  ASSERT_TRUE(e.Pop().has_value());
  EXPECT_TRUE(e.ClearBackpressure());
  EXPECT_EQ(e.state(), ge::EdgeState::kActive);
  EXPECT_EQ(e.metrics().Load().would_block_count, 1U);
}

TEST(EdgeChannelTest, DropOldestKeepsFifoAndNeverEvictsEos) {
  auto e = MakeEdge(ge::DropPolicy::kDropOldest, 3);
  EXPECT_EQ(e.Push(Data(1)), ge::PushOutcome::kAccepted);
  EXPECT_EQ(e.Push(Data(2)), ge::PushOutcome::kAccepted);
  EXPECT_EQ(e.Push(Data(3)), ge::PushOutcome::kAccepted);
  EXPECT_EQ(e.Push(Data(4)), ge::PushOutcome::kAcceptedDroppedOldest);
  EXPECT_EQ((*e.Pop())->header.seq, 2U);
  EXPECT_EQ((*e.Pop())->header.seq, 3U);
  EXPECT_EQ((*e.Pop())->header.seq, 4U);
  EXPECT_EQ(e.metrics().Load().drop_count, 1U);

  // EOS at the head survives eviction; the packet after it is dropped.
  EXPECT_EQ(e.Push(Data(10, 0, GE_PACKET_FLAG_EOS)), ge::PushOutcome::kAccepted);
  EXPECT_EQ(e.Push(Data(11)), ge::PushOutcome::kAccepted);
  EXPECT_EQ(e.Push(Data(12)), ge::PushOutcome::kAccepted);
  EXPECT_EQ(e.Push(Data(13)), ge::PushOutcome::kAcceptedDroppedOldest);
  EXPECT_EQ((*e.Pop())->header.seq, 10U);
  EXPECT_EQ((*e.Pop())->header.seq, 12U);
  EXPECT_EQ((*e.Pop())->header.seq, 13U);
}

TEST(EdgeChannelTest, DropNewestAndEosBlocks) {
  auto e = MakeEdge(ge::DropPolicy::kDropNewest);
  EXPECT_EQ(e.Push(Data(1)), ge::PushOutcome::kAccepted);
  EXPECT_EQ(e.Push(Data(2)), ge::PushOutcome::kAccepted);
  EXPECT_EQ(e.Push(Data(3)), ge::PushOutcome::kDroppedNewest);
  EXPECT_EQ(e.state(), ge::EdgeState::kActive);
  // EOS is never dropped: reports would-block instead.
  EXPECT_EQ(e.Push(Data(4, 0, GE_PACKET_FLAG_EOS)), ge::PushOutcome::kWouldBlock);
  EXPECT_EQ(e.state(), ge::EdgeState::kBackpressured);
  ASSERT_TRUE(e.Pop().has_value());
  EXPECT_EQ(e.Push(Data(4, 0, GE_PACKET_FLAG_EOS)), ge::PushOutcome::kAccepted);
}

TEST(EdgeChannelTest, DrainingAndRetired) {
  auto e = MakeEdge(ge::DropPolicy::kBlock, 4);
  EXPECT_EQ(e.Push(Data(1)), ge::PushOutcome::kAccepted);
  e.MarkDraining();
  EXPECT_EQ(e.Push(Data(2)), ge::PushOutcome::kCancelled);
  EXPECT_EQ(e.metrics().Load().cancelled_count, 1U);
  EXPECT_TRUE(e.Pop().has_value());  // consuming still allowed
  EXPECT_EQ(e.Push(Data(3)), ge::PushOutcome::kCancelled);
  e.MarkRetired(false);
  EXPECT_EQ(e.state(), ge::EdgeState::kRetired);

  auto f = MakeEdge(ge::DropPolicy::kBlock, 4);
  auto pkt = Data(1);
  EXPECT_EQ(f.Push(pkt), ge::PushOutcome::kAccepted);
  EXPECT_EQ(pkt.use_count(), 2);
  f.MarkRetired(true);
  EXPECT_EQ(f.depth(), 0U);
  EXPECT_EQ(pkt.use_count(), 1);  // refs released by fast clear
}

TEST(EdgeChannelTest, SpscUnderThreads) {
  auto e = MakeEdge(ge::DropPolicy::kBlock, 16);
  constexpr int kN = 20000;
  std::thread producer([&] {
    for (int i = 1; i <= kN;) {
      if (e.Push(Data(static_cast<ge::PacketSeq>(i))) == ge::PushOutcome::kAccepted) {
        ++i;
      } else {
        (void)e.ClearBackpressure();
        std::this_thread::yield();
      }
    }
  });
  ge::PacketSeq expect = 1;
  while (expect <= kN) {
    if (auto p = e.Pop()) {
      ASSERT_EQ((*p)->header.seq, expect);
      ++expect;
      (void)e.ClearBackpressure();
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  EXPECT_EQ(e.metrics().Load().popped, static_cast<std::uint64_t>(kN));
}

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

}  // namespace
