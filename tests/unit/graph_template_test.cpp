#include <ge/cpp/batch_runner.h>
#include <ge/cpp/engine.h>
#include <ge/cpp/graph_template.h>

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "test_operators.h"

namespace {

using namespace ge;
using namespace ge::test;

OperatorKey Op(const char* text) { return *OperatorKey::Parse(text); }

GraphSpec Skeleton() {
  GraphSpec g("batch");
  NodeSpec src;
  src.id = "src";
  src.op = Op("Src@1.0.0");
  src.options = JsonValue(JsonObject{{"count", JsonValue("${count}")},
                                     {"label", JsonValue("item ${name} of ${count}")}});
  NodeSpec sink;
  sink.id = "sink";
  sink.op = Op("Sink@1.0.0");
  sink.options = JsonValue(JsonObject{{"path", JsonValue("${dir}/${name}.bin")}});
  EXPECT_TRUE(g.AddNode(std::move(src)).ok());
  EXPECT_TRUE(g.AddNode(std::move(sink)).ok());
  EdgeSpec e;
  e.id = "e0";
  e.from = {"src", "out"};
  e.to = {"sink", "in"};
  EXPECT_TRUE(g.AddEdge(std::move(e)).ok());
  return g;
}

std::vector<TemplateParameter> Params() {
  TemplateParameter count{.name = "count", .required = false, .default_value = JsonValue(5)};
  TemplateParameter name{.name = "name"};
  TemplateParameter dir{.name = "dir"};
  return {count, name, dir};
}

TEST(GraphTemplateTest, DeclarationAndPlaceholdersMustMatch) {
  auto ok = GraphTemplate::Create(Skeleton(), Params());
  ASSERT_TRUE(ok.ok()) << ok.status().ToString();

  auto missing = GraphTemplate::Create(Skeleton(), {TemplateParameter{.name = "count", .required = false, .default_value = JsonValue(5)}});
  EXPECT_EQ(missing.status().code(), GE_STATUS_INVALID_ARGUMENT);  // ${name}/${dir} undeclared

  std::vector<TemplateParameter> extra = Params();
  extra.push_back(TemplateParameter{.name = "unused"});
  EXPECT_EQ(GraphTemplate::Create(Skeleton(), extra).status().code(), GE_STATUS_INVALID_ARGUMENT);

  std::vector<TemplateParameter> dup = Params();
  dup.push_back(dup.front());
  EXPECT_EQ(GraphTemplate::Create(Skeleton(), dup).status().code(), GE_STATUS_INVALID_ARGUMENT);

  std::vector<TemplateParameter> bad_default = Params();
  bad_default[0].default_value = JsonValue();
  EXPECT_EQ(GraphTemplate::Create(Skeleton(), bad_default).status().code(), GE_STATUS_INVALID_ARGUMENT);
}

TEST(GraphTemplateTest, InstantiateSubstitutesTypedAndEmbedded) {
  auto t = GraphTemplate::Create(Skeleton(), Params());
  ASSERT_TRUE(t.ok());
  auto spec = t->Instantiate(JsonValue(JsonObject{{"name", JsonValue("a")}, {"dir", JsonValue("/out")},
                                                  {"count", JsonValue(7)}}),
                             "a");
  ASSERT_TRUE(spec.ok()) << spec.status().ToString();
  EXPECT_EQ(spec->name(), "batch#a");
  const NodeSpec* src = spec->FindNode("src");
  ASSERT_NE(src, nullptr);
  EXPECT_EQ(src->options.GetInteger("count"), 7);  // whole-string placeholder keeps the type
  EXPECT_EQ(src->options.GetString("label"), "item a of 7");
  EXPECT_EQ(spec->FindNode("sink")->options.GetString("path"), "/out/a.bin");

  // Defaults fill absent optional parameters; the skeleton is untouched.
  auto with_default = t->Instantiate(JsonValue(JsonObject{{"name", JsonValue("b")}, {"dir", JsonValue("/out")}}));
  ASSERT_TRUE(with_default.ok());
  EXPECT_EQ(with_default->FindNode("src")->options.GetInteger("count"), 5);
  EXPECT_EQ(with_default->name(), "batch");
  EXPECT_EQ(t->skeleton().FindNode("src")->options.GetString("count"), "${count}");

  EXPECT_EQ(t->Instantiate(JsonValue(JsonObject{{"dir", JsonValue("/out")}})).status().code(),
            GE_STATUS_INVALID_ARGUMENT);  // missing required "name"
  EXPECT_EQ(t->Instantiate(JsonValue(JsonObject{{"name", JsonValue("a")}, {"dir", JsonValue("/out")},
                                                {"nope", JsonValue(1)}}))
                .status()
                .code(),
            GE_STATUS_INVALID_ARGUMENT);  // undeclared argument
  EXPECT_EQ(t->Instantiate(JsonValue(JsonObject{{"name", JsonValue(JsonArray{})}, {"dir", JsonValue("/out")}}))
                .status()
                .code(),
            GE_STATUS_INVALID_ARGUMENT);  // array embedded in a string
  EXPECT_EQ(t->Instantiate(JsonValue("not an object")).status().code(), GE_STATUS_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// BatchRunner on an inline engine
// ---------------------------------------------------------------------------

struct BatchFixture {
  std::shared_ptr<HostBufferPool> pool = HostBufferPool::Create();
  std::shared_ptr<BuiltinOperatorFactory> factory = std::make_shared<BuiltinOperatorFactory>();
  std::vector<std::shared_ptr<Operator>> keep;
  std::atomic<int> creations{0};
  std::map<std::string, std::int64_t> counts;  // sink path -> packets seen
  std::atomic<int> fail_first{0};  // items whose source fails on creation attempt 1

  BatchFixture() {
    factory->Register(Desc("Src@1.0.0", {}, {BytesPort("out", PortDirection::kOutput, true, PortCardinality::kMulti)}, true, 1),
                      [this](const OperatorCreateArgs& a) {
                        ++creations;
                        const auto n = a.options.GetInteger("count").value_or(3);
                        return Keep(keep, std::make_shared<CountingSource>(n, pool));
                      });
    factory->Register(Desc("Sink@1.0.0", {BytesPort("in", PortDirection::kInput)}, {}),
                      [this](const OperatorCreateArgs& a) {
                        auto c = std::make_shared<Collector>();
                        collectors[a.options.GetString("path").value_or("?")] = c.get();
                        return Keep(keep, c);
                      });
  }

  std::map<std::string, Collector*> collectors;

  std::unique_ptr<Engine> MakeEngine(std::uint32_t cpu_threads = 0) {
    EngineConfig c;
    c.cpu_threads = cpu_threads;
    c.watchdog_thread = false;
    c.builtin_operators = factory;
    auto e = Engine::Create(std::move(c));
    EXPECT_TRUE(e.ok());
    return std::move(*e);
  }
};

TEST(BatchRunnerTest, RunsItemsThroughOneTemplateWithBoundedConcurrency) {
  BatchFixture f;
  auto engine = f.MakeEngine();
  auto t = GraphTemplate::Create(Skeleton(), Params());
  ASSERT_TRUE(t.ok());
  ASSERT_TRUE(engine->PrevalidateTemplate(*t).ok());

  BatchOptions o;
  o.concurrency = 2;
  BatchRunner runner(*engine, std::move(*t), o);
  std::vector<BatchItem> items;
  for (int i = 0; i < 5; ++i) {
    items.push_back(BatchItem{"v" + std::to_string(i),
                              JsonValue(JsonObject{{"name", JsonValue("v" + std::to_string(i))},
                                                   {"dir", JsonValue("/out")},
                                                   {"count", JsonValue(4 + i)}})});
  }
  auto report = runner.Run(items);
  ASSERT_TRUE(report.ok()) << report.status().ToString();
  EXPECT_TRUE(report->ok());
  EXPECT_EQ(report->succeeded, 5U);
  ASSERT_EQ(report->items.size(), 5U);
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(report->items[i].status.ok()) << report->items[i].status.ToString();
    EXPECT_EQ(report->items[i].attempts, 1);
    Collector* c = f.collectors["/out/v" + std::to_string(i) + ".bin"];
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->Seqs().size(), static_cast<std::size_t>(4 + i)) << i;  // per-item count reached the right sink
  }
  EXPECT_TRUE(engine->Sessions().empty());  // every session destroyed
  EXPECT_EQ(f.creations.load(), 5);

  EXPECT_EQ(runner.Run({BatchItem{"a"}, BatchItem{"a"}}).status().code(), GE_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(runner.Run({BatchItem{}}).status().code(), GE_STATUS_INVALID_ARGUMENT);
}

TEST(BatchRunnerTest, FailedItemIsRetriedAndOthersUnaffected) {
  BatchFixture f;
  f.factory->Register(Desc("Flaky@1.0.0", {}, {BytesPort("out", PortDirection::kOutput, true, PortCardinality::kMulti)}, true, 1),
                      [&f](const OperatorCreateArgs&) -> std::unique_ptr<Operator> {
                        if (f.fail_first.fetch_sub(1) > 0) return std::make_unique<FailsToOpen>();
                        return Keep(f.keep, std::make_shared<CountingSource>(3, f.pool));
                      });
  auto engine = f.MakeEngine();

  GraphSpec g("flaky");
  NodeSpec src;
  src.id = "src";
  src.op = Op("Flaky@1.0.0");
  src.options = JsonValue(JsonObject{{"tag", JsonValue("${name}")}});
  NodeSpec sink;
  sink.id = "sink";
  sink.op = Op("Sink@1.0.0");
  sink.options = JsonValue(JsonObject{{"path", JsonValue("${name}")}});
  ASSERT_TRUE(g.AddNode(std::move(src)).ok());
  ASSERT_TRUE(g.AddNode(std::move(sink)).ok());
  EdgeSpec e;
  e.from = {"src", "out"};
  e.to = {"sink", "in"};
  ASSERT_TRUE(g.AddEdge(std::move(e)).ok());
  auto t = GraphTemplate::Create(std::move(g), {TemplateParameter{.name = "name"}});
  ASSERT_TRUE(t.ok());

  f.fail_first = 1;  // exactly the first creation fails to open
  BatchOptions o;
  o.concurrency = 1;
  o.max_attempts = 2;
  BatchRunner runner(*engine, std::move(*t), o);
  auto report = runner.Run({BatchItem{"x", JsonValue(JsonObject{{"name", JsonValue("x")}})},
                            BatchItem{"y", JsonValue(JsonObject{{"name", JsonValue("y")}})}});
  ASSERT_TRUE(report.ok()) << report.status().ToString();
  EXPECT_TRUE(report->ok());
  EXPECT_EQ(report->items[0].attempts, 2);  // failed once, retried, succeeded
  EXPECT_EQ(report->items[1].attempts, 1);
  EXPECT_TRUE(engine->Sessions().empty());
}

// GM-3 / RES-1: an item that blows its time budget is fast-stopped, retried,
// and every attempt returns its session and resource lease -- nothing may
// accumulate across retries (the long-batch counterpart of the 24h "no leak"
// acceptance in docs/09). Threaded engine: with an inline executor a
// never-ending stream would spin RunPending() forever inside Pump(), so the
// timeout could never interrupt it -- which is exactly why timeouts matter
// on real deployments.
TEST(BatchRunnerTest, ItemTimeoutRetriesAndReleasesEveryLease) {
  BatchFixture f;
  auto engine = f.MakeEngine(2);
  ResourceLedger* ledger = engine->resource_ledger();
  ASSERT_NE(ledger, nullptr);
  auto t = GraphTemplate::Create(Skeleton(), Params());
  ASSERT_TRUE(t.ok());

  BatchOptions o;
  o.concurrency = 2;
  o.max_attempts = 2;
  o.item_timeout = std::chrono::milliseconds(50);
  BatchRunner runner(*engine, std::move(*t), o);
  // "x" can never finish inside 50ms (2^40 packets); "y" is trivial and must
  // be unaffected by its neighbour timing out (SC-7: no preemption of others).
  auto report = runner.Run(
      {BatchItem{"x", JsonValue(JsonObject{{"name", JsonValue("x")}, {"dir", JsonValue("/out")},
                                           {"count", JsonValue(std::int64_t{1} << 40)}})},
       BatchItem{"y", JsonValue(JsonObject{{"name", JsonValue("y")}, {"dir", JsonValue("/out")},
                                           {"count", JsonValue(3)}})}});
  ASSERT_TRUE(report.ok()) << report.status().ToString();
  EXPECT_FALSE(report->ok());
  EXPECT_EQ(report->failed, 1U);
  EXPECT_EQ(report->succeeded, 1U);
  const BatchItemResult& x = report->items[0];
  EXPECT_EQ(x.attempts, 2);  // timed out, retried, timed out again
  EXPECT_EQ(x.status.code(), GE_STATUS_CANCELLED) << x.status.ToString();
  EXPECT_TRUE(report->items[1].status.ok()) << report->items[1].status.ToString();
  ASSERT_NE(f.collectors["/out/y.bin"], nullptr);
  EXPECT_EQ(f.collectors["/out/y.bin"]->Seqs().size(), 3U);
  // Every attempt's session is gone and returned its lease (RES-1); no
  // buffers left in flight either.
  EXPECT_TRUE(engine->Sessions().empty());
  EXPECT_EQ(ledger->live_leases(), 0U);
  EXPECT_EQ(f.pool->stats().live_buffers, 0U);
}

TEST(BatchRunnerTest, ExhaustedAttemptsReportFailure) {
  BatchFixture f;
  f.factory->Register(Desc("Broken@1.0.0", {}, {BytesPort("out", PortDirection::kOutput, true, PortCardinality::kMulti)}, true, 1),
                      [](const OperatorCreateArgs&) { return std::make_unique<FailsToOpen>(); });
  auto engine = f.MakeEngine();
  GraphSpec g("broken");
  NodeSpec src;
  src.id = "src";
  src.op = Op("Broken@1.0.0");
  src.options = JsonValue(JsonObject{{"tag", JsonValue("${name}")}});
  NodeSpec sink;
  sink.id = "sink";
  sink.op = Op("Sink@1.0.0");
  sink.options = JsonValue(JsonObject{{"path", JsonValue("${name}")}});
  ASSERT_TRUE(g.AddNode(std::move(src)).ok());
  ASSERT_TRUE(g.AddNode(std::move(sink)).ok());
  EdgeSpec e;
  e.from = {"src", "out"};
  e.to = {"sink", "in"};
  ASSERT_TRUE(g.AddEdge(std::move(e)).ok());
  auto t = GraphTemplate::Create(std::move(g), {TemplateParameter{.name = "name"}});
  ASSERT_TRUE(t.ok());
  BatchOptions o;
  o.max_attempts = 2;
  BatchRunner runner(*engine, std::move(*t), o);
  auto report = runner.Run({BatchItem{"x", JsonValue(JsonObject{{"name", JsonValue("x")}})}});
  ASSERT_TRUE(report.ok());
  EXPECT_FALSE(report->ok());
  EXPECT_EQ(report->failed, 1U);
  EXPECT_EQ(report->items[0].attempts, 2);
  EXPECT_FALSE(report->items[0].status.ok());
  EXPECT_TRUE(engine->Sessions().empty());
}

}  // namespace
