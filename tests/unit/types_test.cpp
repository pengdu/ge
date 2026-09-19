#include <ge/cpp/types.h>

#include <gtest/gtest.h>

namespace {

TEST(StatusTest, DefaultIsOk) {
  const ge::Status status;
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(status.code(), GE_STATUS_OK);
  EXPECT_FALSE(status.retryable());
  EXPECT_EQ(status.ToString(), "OK");
}

TEST(StatusTest, FactoriesCarryCodeAndRetryability) {
  EXPECT_EQ(ge::Status::VersionConflict("x").code(), GE_STATUS_VERSION_CONFLICT);
  EXPECT_TRUE(ge::Status::VersionConflict("x").retryable());
  EXPECT_FALSE(ge::Status::CapabilityConflict("x").retryable());
  EXPECT_TRUE(ge::Status::ResourceExhausted("x").retryable());
  EXPECT_EQ(ge::Status::GraphInvalid("bad", "{\"k\":1}").context_json(),
            "{\"k\":1}");
  EXPECT_EQ(ge::Status::Internal("boom").ToString(), "INTERNAL: boom");
}

TEST(StatusTest, EveryCodeHasName) {
  for (int c = GE_STATUS_OK; c <= GE_STATUS_INTERNAL; ++c) {
    EXPECT_NE(ge::Status::CodeName(static_cast<ge_status_code>(c)), "UNKNOWN");
  }
}

TEST(ResultTest, ValueAndError) {
  ge::Result<int> ok(42);
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ(*ok, 42);

  ge::Result<int> err(ge::Status::NotFound("nope"));
  ASSERT_FALSE(err.ok());
  EXPECT_EQ(err.status().code(), GE_STATUS_NOT_FOUND);

  ge::Result<void> vok;
  EXPECT_TRUE(vok.ok());
  ge::Result<void> verr(ge::Status::Cancelled("c"));
  EXPECT_FALSE(verr.ok());
}

TEST(ResultTest, OkStatusWithoutValueBecomesInternal) {
  ge::Result<int> bad(ge::Status::Ok());
  EXPECT_FALSE(bad.ok());
  EXPECT_EQ(bad.status().code(), GE_STATUS_INTERNAL);
}

TEST(OperatorKeyTest, ParseAndFormat) {
  const auto key = ge::OperatorKey::Parse("video.scale@1.2.0");
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(key->type_name, "video.scale");
  EXPECT_EQ(key->semantic_version, "1.2.0");
  EXPECT_EQ(key->ToString(), "video.scale@1.2.0");
  EXPECT_FALSE(ge::OperatorKey::Parse("no_at").has_value());
  EXPECT_FALSE(ge::OperatorKey::Parse("@1").has_value());
  EXPECT_FALSE(ge::OperatorKey::Parse("x@").has_value());
}

}  // namespace
