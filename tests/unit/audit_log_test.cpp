#include <ge/cpp/audit_log.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using ge::AuditFilter;
using ge::AuditLog;
using ge::AuditRecord;
using ge::JsonObject;
using ge::JsonValue;

TEST(AuditLogTest, RedactsSensitiveKeysUrlsAndLargePayloads) {
  JsonObject inner;
  inner["api_key"] = JsonValue("abc");
  inner["Authorization"] = JsonValue("Bearer x");
  inner["url"] = JsonValue("rtmp://user:pw@host:1935/live/stream?token=ttt#frag");
  inner["plain"] = JsonValue("ok");
  JsonObject o;
  o["password"] = JsonValue("hunter2");
  o["nested"] = JsonValue(inner);
  o["blob"] = JsonValue(std::string(1000, 'x'));
  ge::JsonArray arr;
  for (int i = 0; i < 40; ++i) arr.push_back(JsonValue(i));
  o["list"] = JsonValue(arr);
  o["model"] = JsonValue("/models/a.onnx");

  const JsonValue r = ge::Redact(JsonValue(o));
  const auto& ro = r.as_object();
  EXPECT_EQ(ro.at("password").as_string(), "***");
  const auto& n = ro.at("nested").as_object();
  EXPECT_EQ(n.at("api_key").as_string(), "***");
  EXPECT_EQ(n.at("Authorization").as_string(), "***");
  EXPECT_EQ(n.at("url").as_string(), "rtmp://***@host:1935/live/stream");
  EXPECT_EQ(n.at("plain").as_string(), "ok");
  EXPECT_LT(ro.at("blob").as_string().size(), 300U);
  EXPECT_NE(ro.at("blob").as_string().find("1000 chars"), std::string::npos);
  EXPECT_EQ(ro.at("list").as_array().size(), 33U);  // 32 + "<+8 more>"
  EXPECT_EQ(ro.at("model").as_string(), "/models/a.onnx");
  // Original untouched (copy-on-write).
  EXPECT_EQ(o.at("password").as_string(), "hunter2");

  EXPECT_EQ(ge::RedactUrl("https://h/p?q=1"), "https://h/p");
  EXPECT_EQ(ge::RedactUrl("not a url"), "not a url");
  EXPECT_TRUE(ge::IsSensitiveKey("X-Secret-Header"));
  EXPECT_FALSE(ge::IsSensitiveKey("node"));
}

TEST(AuditLogTest, AppendRedactsStampsRingsAndCallsSink) {
  AuditLog log(3);
  std::vector<std::uint64_t> seen;
  log.SetSink([&](const AuditRecord& r) {
    seen.push_back(r.audit_id);
    EXPECT_EQ(r.digest.as_object().at("token").as_string(), "***");  // sink sees redacted
    throw std::runtime_error("consumer bug");                          // swallowed
  });
  for (int i = 0; i < 5; ++i) {
    AuditRecord r;
    r.operation = i % 2 == 0 ? "session.create" : "mutation.apply";
    r.session_id = static_cast<ge::SessionId>(i + 1);
    r.caller.caller_id = "c";
    r.caller.request_id = "r" + std::to_string(i);
    r.result = i == 3 ? GE_STATUS_RESOURCE_EXHAUSTED : GE_STATUS_OK;
    r.digest = JsonValue(JsonObject{{"token", JsonValue("t")}, {"i", JsonValue(i)}});
    log.Append(std::move(r));
  }
  EXPECT_EQ(seen, (std::vector<std::uint64_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(log.size(), 3U);
  EXPECT_EQ(log.dropped(), 2U);
  EXPECT_EQ(log.last_audit_id(), 5U);

  auto all = log.Query({});
  ASSERT_EQ(all.size(), 3U);
  EXPECT_EQ(all[0].audit_id, 3U);
  EXPECT_GT(all[0].timestamp_ns, 0);
  EXPECT_EQ(all[0].digest.as_object().at("token").as_string(), "***");

  AuditFilter f;
  f.failures_only = true;
  auto failures = log.Query(f);
  ASSERT_EQ(failures.size(), 1U);
  EXPECT_EQ(failures[0].session_id, 4U);
  EXPECT_EQ(failures[0].result, GE_STATUS_RESOURCE_EXHAUSTED);

  f = {};
  f.operation = "session.create";
  EXPECT_EQ(log.Query(f).size(), 2U);
  f = {};
  f.request_id = "r4";
  ASSERT_EQ(log.Query(f).size(), 1U);
  f = {};
  f.after_audit_id = 4;
  ASSERT_EQ(log.Query(f).size(), 1U);
  EXPECT_EQ(log.Query(f)[0].audit_id, 5U);
  f = {};
  f.limit = 2;
  EXPECT_EQ(log.Query(f).size(), 2U);

  const JsonValue j = log.QueryJson({});
  EXPECT_EQ(j.as_object().at("records").as_array().size(), 3U);
  EXPECT_EQ(j.as_object().at("dropped").as_integer(), 2);
  const auto& first = j.as_object().at("records").as_array()[0].as_object();
  EXPECT_EQ(first.at("operation").as_string(), "session.create");
  EXPECT_EQ(first.at("result").as_string(), "OK");
  EXPECT_EQ(first.at("caller_id").as_string(), "c");
}

TEST(AuditLogTest, FilterFromJson) {
  auto f = AuditFilter::FromJson(*ge::ParseJson(
      R"({"session_id":3,"operation":"x","caller_id":"c","request_id":"r","failures_only":true,"since_ns":5,"after_audit_id":7,"limit":9})").value);
  ASSERT_TRUE(f.ok());
  EXPECT_EQ(*f->session_id, 3U);
  EXPECT_EQ(*f->operation, "x");
  EXPECT_TRUE(f->failures_only);
  EXPECT_EQ(f->since_ns, 5);
  EXPECT_EQ(f->after_audit_id, 7U);
  EXPECT_EQ(f->limit, 9U);
  EXPECT_TRUE(AuditFilter::FromJson(JsonValue()).ok());
  EXPECT_FALSE(AuditFilter::FromJson(JsonValue(1)).ok());
  EXPECT_FALSE(AuditFilter::FromJson(*ge::ParseJson(R"({"limit":-1})").value).ok());
}

}  // namespace
