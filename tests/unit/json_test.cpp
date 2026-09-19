#include <ge/cpp/json.h>

#include <gtest/gtest.h>

namespace {

TEST(JsonTest, ParsesScalars) {
  EXPECT_TRUE(ge::ParseJson("null").value->is_null());
  EXPECT_TRUE(ge::ParseJson("true").value->as_bool());
  EXPECT_EQ(ge::ParseJson("42").value->as_integer(), 42);
  EXPECT_TRUE(ge::ParseJson("42").value->is_integer());
  EXPECT_DOUBLE_EQ(ge::ParseJson("4.5").value->as_number(), 4.5);
  EXPECT_FALSE(ge::ParseJson("4.5").value->is_integer());
  EXPECT_EQ(ge::ParseJson("\"a\\nb\"").value->as_string(), "a\nb");
}

TEST(JsonTest, PreservesInt64Precision) {
  const auto v = ge::ParseJson("9007199254740993");
  ASSERT_TRUE(v.ok());
  EXPECT_EQ(v.value->as_integer(), 9007199254740993LL);
}

TEST(JsonTest, ParsesNested) {
  const auto r = ge::ParseJson(R"({"a":[1,{"b":"c"}],"d":{}})");
  ASSERT_TRUE(r.ok()) << r.error;
  const ge::JsonValue* a = r.value->Find("a");
  ASSERT_NE(a, nullptr);
  ASSERT_EQ(a->as_array().size(), 2U);
  EXPECT_EQ(a->as_array()[1].GetString("b"), "c");
  EXPECT_TRUE(r.value->Find("d")->is_object());
}

TEST(JsonTest, RejectsMalformed) {
  EXPECT_FALSE(ge::ParseJson("{").ok());
  EXPECT_FALSE(ge::ParseJson("[1,]").ok());
  EXPECT_FALSE(ge::ParseJson("{\"a\":1,\"a\":2}").ok());
  EXPECT_FALSE(ge::ParseJson("1 2").ok());
  EXPECT_FALSE(ge::ParseJson("").ok());
}

TEST(JsonTest, SerializeIsDeterministic) {
  const auto a = ge::ParseJson(R"({"z":1,"a":[true,null,"x\"y"],"m":{"k":2.5}})");
  const auto b = ge::ParseJson(R"({"a":[true,null,"x\"y"],"m":{"k":2.5},"z":1})");
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(a.value->Serialize(), b.value->Serialize());
  EXPECT_EQ(a.value->Serialize(), R"({"a":[true,null,"x\"y"],"m":{"k":2.5},"z":1})");
  EXPECT_EQ(*a.value, *b.value);
}

TEST(JsonTest, RoundTrip) {
  const std::string text = R"({"n":-7,"f":0.125,"s":"\u00e9","e":[]})";
  const auto v = ge::ParseJson(text);
  ASSERT_TRUE(v.ok());
  const auto again = ge::ParseJson(v.value->Serialize());
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(*v.value, *again.value);
}

TEST(JsonTest, RejectsExcessiveNesting) {
  std::string deep(300, '[');
  deep += std::string(300, ']');
  EXPECT_FALSE(ge::ParseJson(deep).ok());
}

}  // namespace
