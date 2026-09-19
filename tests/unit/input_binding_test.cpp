#include <ge/cpp/input_binding.h>

#include <gtest/gtest.h>

namespace {

ge::PacketRef Data(ge::PacketSeq seq, std::int64_t pts = 0, std::uint32_t flags = 0) {
  auto p = std::make_shared<ge::Packet>();
  p->header.seq = seq;
  p->header.pts_ns = pts;
  p->header.flags = flags;
  return p;
}

ge::EdgeChannelRef Edge(const char* port, std::uint32_t cap = 8) {
  ge::EdgeConfig cfg;
  cfg.capacity = cap;
  cfg.policy = ge::DropPolicy::kBlock;
  return std::make_shared<ge::EdgeChannel>(1, port, ge::ConnectionContract{}, cfg,
                                           ge::PortRef{"u", "o"}, ge::PortRef{"n", port});
}

TEST(InputBindingTest, AnyTriggersOnFirstRequiredPortWithData) {
  auto a = Edge("a"), b = Edge("b"), opt = Edge("opt");
  ge::InputBinding in({{"a", a, true, {}}, {"b", b, true, {}}, {"opt", opt, false, {}}}, ge::SyncPolicy::kAny);
  EXPECT_FALSE(in.MayBeReady());
  EXPECT_FALSE(in.TryAcquire().has_value());
  ASSERT_EQ(b->Push(Data(1)), ge::PushOutcome::kAccepted);
  ASSERT_EQ(a->Push(Data(2)), ge::PushOutcome::kAccepted);
  EXPECT_TRUE(in.MayBeReady());
  auto batch = in.TryAcquire();
  ASSERT_TRUE(batch.has_value());
  ASSERT_EQ(batch->ports.size(), 1U);
  EXPECT_EQ(batch->ports[0], "a");  // declaration order wins
  EXPECT_EQ(batch->packets[0]->header.seq, 2U);
  batch = in.TryAcquire();
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->ports[0], "b");
  ASSERT_EQ(opt->Push(Data(3)), ge::PushOutcome::kAccepted);
  batch = in.TryAcquire();
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->ports[0], "opt");  // optional only when no required data
}

TEST(InputBindingTest, AnyEosRules) {
  auto a = Edge("a"), b = Edge("b");
  ge::InputBinding in({{"a", a, true, {}}, {"b", b, true, {}}}, ge::SyncPolicy::kAny);
  ASSERT_EQ(a->Push(Data(1, 0, GE_PACKET_FLAG_EOS)), ge::PushOutcome::kAccepted);
  ASSERT_EQ(b->Push(Data(2)), ge::PushOutcome::kAccepted);
  auto batch = in.TryAcquire();
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->ports[0], "b");
  EXPECT_TRUE(in.eos_ports().contains("a"));
  EXPECT_FALSE(in.InputEnded());
  ASSERT_EQ(b->Push(Data(3, 0, GE_PACKET_FLAG_EOS)), ge::PushOutcome::kAccepted);
  EXPECT_FALSE(in.TryAcquire().has_value());
  EXPECT_TRUE(in.InputEnded());
  // Data after EOS on a port is dropped and counted.
  ASSERT_EQ(a->Push(Data(4)), ge::PushOutcome::kAccepted);
  EXPECT_FALSE(in.TryAcquire().has_value());
  EXPECT_EQ(in.stale_dropped(), 1U);
}

TEST(InputBindingTest, LatestConsumesTriggerAndNewestOfOthers) {
  auto v = Edge("video"), o = Edge("overlay");
  ge::InputBinding in({{"video", v, true, {}}, {"overlay", o, false, {}}}, ge::SyncPolicy::kLatest);
  ASSERT_EQ(o->Push(Data(10)), ge::PushOutcome::kAccepted);
  ASSERT_EQ(o->Push(Data(11)), ge::PushOutcome::kAccepted);
  ASSERT_EQ(o->Push(Data(12)), ge::PushOutcome::kAccepted);
  ASSERT_EQ(v->Push(Data(1)), ge::PushOutcome::kAccepted);
  ASSERT_EQ(v->Push(Data(2)), ge::PushOutcome::kAccepted);
  auto batch = in.TryAcquire();
  ASSERT_TRUE(batch.has_value());
  ASSERT_EQ(batch->ports.size(), 2U);
  EXPECT_EQ(batch->ports[0], "video");
  EXPECT_EQ(batch->packets[0]->header.seq, 1U);
  EXPECT_EQ(batch->ports[1], "overlay");
  EXPECT_EQ(batch->packets[1]->header.seq, 12U);
  EXPECT_EQ(in.stale_dropped(), 2U);
  EXPECT_EQ(o->depth(), 0U);
  // No overlay available: trigger alone.
  batch = in.TryAcquire();
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->ports.size(), 1U);
  // Overlay EOS: "no latest value".
  ASSERT_EQ(o->Push(Data(13, 0, GE_PACKET_FLAG_EOS)), ge::PushOutcome::kAccepted);
  ASSERT_EQ(v->Push(Data(3)), ge::PushOutcome::kAccepted);
  batch = in.TryAcquire();
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->ports.size(), 1U);
  EXPECT_TRUE(in.eos_ports().contains("overlay"));
  EXPECT_FALSE(in.InputEnded());
}

TEST(InputBindingTest, AlignedPicksWithinWindowAndDropsLate) {
  auto v = Edge("video"), a = Edge("audio");
  ge::AlignedOptions opts;
  opts.window_ns = 10;
  ge::InputBinding in({{"video", v, true, {}}, {"audio", a, true, {}}}, ge::SyncPolicy::kAligned, opts);
  ASSERT_EQ(v->Push(Data(1, 100)), ge::PushOutcome::kAccepted);
  EXPECT_FALSE(in.TryAcquire().has_value());  // audio missing
  ASSERT_EQ(a->Push(Data(10, 50)), ge::PushOutcome::kAccepted);   // too old -> dropped
  ASSERT_EQ(a->Push(Data(11, 80)), ge::PushOutcome::kAccepted);   // too old -> dropped
  ASSERT_EQ(a->Push(Data(12, 95)), ge::PushOutcome::kAccepted);   // within window
  ASSERT_EQ(a->Push(Data(13, 105)), ge::PushOutcome::kAccepted);
  auto batch = in.TryAcquire();
  ASSERT_TRUE(batch.has_value());
  ASSERT_EQ(batch->ports.size(), 2U);
  EXPECT_EQ(batch->packets[0]->header.seq, 1U);
  EXPECT_EQ(batch->packets[1]->header.seq, 12U);
  EXPECT_EQ(in.late_dropped(), 2U);
  EXPECT_EQ(a->depth(), 1U);  // 13 kept for the next window

  // Reference port configured: target follows it even if not earliest.
  ge::AlignedOptions ref = opts;
  ref.reference_port = "audio";
  ge::InputBinding in2({{"video", v, true, {}}, {"audio", a, true, {}}}, ge::SyncPolicy::kAligned, ref);
  ASSERT_EQ(v->Push(Data(2, 104)), ge::PushOutcome::kAccepted);
  batch = in2.TryAcquire();
  ASSERT_TRUE(batch.has_value());
  ASSERT_EQ(batch->ports.size(), 2U);
  EXPECT_EQ(batch->ports[0], "video");  // declaration order
  EXPECT_EQ(batch->packets[0]->header.seq, 2U);
  EXPECT_EQ(batch->packets[1]->header.seq, 13U);

  // Reference EOS ends input immediately and drops leftovers.
  ASSERT_EQ(v->Push(Data(3, 200)), ge::PushOutcome::kAccepted);
  ASSERT_EQ(a->Push(Data(14, 0, GE_PACKET_FLAG_EOS)), ge::PushOutcome::kAccepted);
  EXPECT_FALSE(in2.TryAcquire().has_value());
  EXPECT_TRUE(in2.InputEnded());
  EXPECT_EQ(v->depth(), 0U);
}

TEST(InputBindingTest, EventPacketsBypassSync) {
  auto v = Edge("video");
  ge::InputBinding in({{"video", v, true, {}}}, ge::SyncPolicy::kAny);
  ASSERT_EQ(v->Push(Data(1, 0, GE_PACKET_FLAG_EVENT)), ge::PushOutcome::kAccepted);
  auto batch = in.TryAcquire();
  ASSERT_TRUE(batch.has_value());
  EXPECT_TRUE(batch->ports.empty());
  ASSERT_EQ(batch->events.size(), 1U);
  ASSERT_EQ(v->Push(Data(2, 0, GE_PACKET_FLAG_EVENT)), ge::PushOutcome::kAccepted);
  ASSERT_EQ(v->Push(Data(3)), ge::PushOutcome::kAccepted);
  batch = in.TryAcquire();
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->events.size(), 1U);
  EXPECT_EQ(batch->packets.size(), 1U);
}

}  // namespace
