#include <ge/cpp/graph_diff.h>

#include <gtest/gtest.h>

#include <ge/cpp/mutation_applier.h>

namespace {

using NC = ge::GraphDiff::NodeChange;
using EC = ge::GraphDiff::EdgeChange;

ge::OperatorKey Op(const char* text) { return *ge::OperatorKey::Parse(text); }

// src.out -> a.in (e0) ; a.out -> sink.in (e1) ; a.out -> sink2.in (e2)
ge::GraphSpec Base() {
  ge::GraphSpec s("g");
  EXPECT_TRUE(s.AddNode({.id = "src", .op = Op("Src@1.0.0")}).ok());
  EXPECT_TRUE(s.AddNode({.id = "a", .op = Op("Pass@1.0.0")}).ok());
  EXPECT_TRUE(s.AddNode({.id = "sink", .op = Op("Sink@1.0.0")}).ok());
  EXPECT_TRUE(s.AddNode({.id = "sink2", .op = Op("Sink@1.0.0")}).ok());
  EXPECT_TRUE(s.AddEdge({.id = "e0", .from = {"src", "out"}, .to = {"a", "in"}}).ok());
  EXPECT_TRUE(s.AddEdge({.id = "e1", .from = {"a", "out"}, .to = {"sink", "in"}}).ok());
  EXPECT_TRUE(s.AddEdge({.id = "e2", .from = {"a", "out"}, .to = {"sink2", "in"}}).ok());
  return s;
}

ge::ConnectionContract Bytes(std::uint64_t targets = 1) {
  ge::ConnectionContract c;
  c.logical_type = "Bytes";
  c.target_capability = targets;
  return c;
}

std::map<std::string, ge::ConnectionContract> Contracts(const ge::GraphSpec& s, std::uint64_t targets = 1) {
  std::map<std::string, ge::ConnectionContract> m;
  for (const ge::EdgeSpec& e : s.edges()) m[e.id] = Bytes(targets);
  return m;
}

TEST(GraphDiffTest, IdenticalSpecsAreEmpty) {
  const ge::GraphSpec b = Base();
  const auto d = ge::GraphDiff::Compute(b, Contracts(b), b, Contracts(b));
  EXPECT_TRUE(d.Empty());
  EXPECT_EQ(d.ReusedNodes().size(), 4U);
  EXPECT_EQ(d.ReusedEdges().size(), 3U);
  EXPECT_EQ(d.ToJson().as_object().at("nodes").as_object().size(), 0U);
}

TEST(GraphDiffTest, ConsumerSetFingerprintAloneKeepsEdge) {
  // Adding a fan-out branch changes target_capability of e1/e2's shared
  // contract; that must not recreate the untouched edges (BUG-01).
  const ge::GraphSpec b = Base();
  ge::GraphSpec c = b;
  ASSERT_TRUE(c.AddNode({.id = "sink3", .op = Op("Sink@1.0.0")}).ok());
  ASSERT_TRUE(c.AddEdge({.id = "e3", .from = {"a", "out"}, .to = {"sink3", "in"}}).ok());
  const auto d = ge::GraphDiff::Compute(b, Contracts(b, 1), c, Contracts(c, 2));
  EXPECT_EQ(d.edges.at("e1").change, EC::kKept);
  EXPECT_EQ(d.edges.at("e2").change, EC::kKept);
  EXPECT_EQ(d.edges.at("e3").change, EC::kAdded);
  EXPECT_EQ(d.nodes.at("sink3").change, NC::kAdded);
  EXPECT_EQ(d.nodes.at("a").change, NC::kKept);  // outputs changed, inputs did not
  EXPECT_EQ(d.Nodes(NC::kAdded), std::vector<std::string>{"sink3"});
}

TEST(GraphDiffTest, DataPlaneContractChangeRecreatesEdgeAndUpdatesConsumer) {
  const ge::GraphSpec b = Base();
  auto cc = Contracts(b);
  cc["e0"].memory_kind = ge::MemoryKind::kPinned;
  const auto d = ge::GraphDiff::Compute(b, Contracts(b), b, cc);
  EXPECT_EQ(d.edges.at("e0").change, EC::kRecreated);
  EXPECT_EQ(d.edges.at("e0").reason, "contract");
  EXPECT_EQ(d.nodes.at("a").change, NC::kUpdated);
  EXPECT_NE(d.nodes.at("a").reason.find("input 'in' recreated"), std::string::npos);
  EXPECT_EQ(d.nodes.at("src").change, NC::kKept);
  EXPECT_FALSE(d.ReusedEdges().contains("e0"));
  EXPECT_TRUE(d.ReusedNodes().contains("a"));  // runtime kept, binding rebound
}

TEST(GraphDiffTest, InsertChainRewiresConsumer) {
  const ge::GraphSpec b = Base();
  ge::MutationApplier applier;
  auto r = applier.Apply(b, ge::Mutation()
                                .InsertChain("e0", {{.id = "w", .op = Op("Pass@1.0.0")}},
                                             ge::ChainPorts{.chain_input = "in", .chain_output = "out"})
                                .Build());
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  const auto d = ge::GraphDiff::Compute(b, Contracts(b), r->candidate, Contracts(r->candidate));
  EXPECT_EQ(d.nodes.at("w").change, NC::kAdded);
  EXPECT_EQ(d.edges.at("e0").change, EC::kRemoved);
  EXPECT_EQ(d.nodes.at("a").change, NC::kUpdated);
  EXPECT_NE(d.nodes.at("a").reason.find("input 'in' rewired"), std::string::npos);
  EXPECT_EQ(d.nodes.at("src").change, NC::kKept);
  EXPECT_EQ(d.nodes.at("sink").change, NC::kKept);
  EXPECT_EQ(d.edges.at("e1").change, EC::kKept);
  EXPECT_EQ(d.Edges(EC::kAdded).size(), 2U);
}

TEST(GraphDiffTest, ReplaceNodeRecreatesItsEdges) {
  const ge::GraphSpec b = Base();
  ge::GraphSpec c = b;
  c.FindNode("a")->op = Op("Pass@2.0.0");
  const auto d = ge::GraphDiff::Compute(b, Contracts(b), c, Contracts(c));
  EXPECT_EQ(d.nodes.at("a").change, NC::kReplaced);
  EXPECT_EQ(d.nodes.at("a").reason, "Pass@1.0.0 -> Pass@2.0.0");
  EXPECT_EQ(d.edges.at("e0").change, EC::kRecreated);
  EXPECT_EQ(d.edges.at("e0").reason, "consumer replaced");
  EXPECT_EQ(d.edges.at("e1").change, EC::kRecreated);
  EXPECT_EQ(d.edges.at("e1").reason, "producer replaced");
  EXPECT_FALSE(d.ReusedNodes().contains("a"));
  EXPECT_TRUE(d.ReusedEdges().empty());
  // Consumers of recreated edges are updated (rebound), not recreated.
  EXPECT_EQ(d.nodes.at("sink").change, NC::kUpdated);
  EXPECT_EQ(d.nodes.at("sink2").change, NC::kUpdated);
}

TEST(GraphDiffTest, QueueAndParallelismChanges) {
  const ge::GraphSpec b = Base();
  ge::GraphSpec c = b;
  c.FindEdge("e1")->queue.capacity = 4;
  c.FindNode("src")->parallelism = 2;
  c.FindNode("sink2")->options = ge::JsonValue(ge::JsonObject{{"gain", ge::JsonValue(3)}});
  const auto d = ge::GraphDiff::Compute(b, Contracts(b), c, Contracts(c));
  EXPECT_EQ(d.edges.at("e1").change, EC::kRecreated);
  EXPECT_EQ(d.edges.at("e1").reason, "queue");
  EXPECT_EQ(d.nodes.at("src").change, NC::kUpdated);
  EXPECT_EQ(d.nodes.at("src").reason, "parallelism");
  EXPECT_EQ(d.nodes.at("sink").change, NC::kUpdated);  // its input was recreated
  EXPECT_EQ(d.nodes.at("sink2").change, NC::kKept);    // options are not topology
  EXPECT_FALSE(d.Empty());
  const auto j = d.ToJson();
  EXPECT_EQ(j.as_object().at("edges").as_object().at("e1").GetString("reason"), "queue");
  EXPECT_FALSE(j.as_object().at("nodes").as_object().contains("sink2"));
}

}  // namespace
