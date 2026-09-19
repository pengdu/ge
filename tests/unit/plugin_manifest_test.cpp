#include <ge/cpp/plugin_manifest.h>

#include <gtest/gtest.h>

namespace {

constexpr const char* kManifest = R"({
  "kind": "PluginManifest",
  "schema_version": 1,
  "$id": "ge.dev/schema/plugin/v1",
  "plugin_id": "com.example.vision",
  "library": "libexample_vision.so",
  "abi": {"major": 1, "minor": 0},
  "build_fingerprint": "clang-18-linux-x86_64",
  "dependencies": [{"name": "tensorrt", "version": "8.6"}],
  "resources": ["models/detector.plan"],
  "operators": ["Detector@2.0.0", "Tracker@1.0.0"],
  "max_inference_ms": 100,
  "no_owned_threads": true
})";

TEST(PluginManifestTest, ParsesAndRoundTrips) {
  const auto r = ge::PluginManifest::ParseJson(kManifest);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->plugin_id, "com.example.vision");
  EXPECT_EQ(r->library, "libexample_vision.so");
  EXPECT_EQ(r->abi, (ge::AbiVersion{1, 0}));
  EXPECT_EQ(r->build_fingerprint, "clang-18-linux-x86_64");
  ASSERT_EQ(r->dependencies.size(), 1U);
  EXPECT_EQ(r->dependencies[0].version, "8.6");
  EXPECT_EQ(r->resources.at(0), "models/detector.plan");
  ASSERT_EQ(r->operators.size(), 2U);
  EXPECT_EQ(r->operators[1].ToString(), "Tracker@1.0.0");
  EXPECT_EQ(r->max_inference_ms, 100U);
  EXPECT_TRUE(r->no_owned_threads);

  const std::string once = r->Serialize();
  const auto again = ge::PluginManifest::ParseJson(once);
  ASSERT_TRUE(again.ok()) << again.status().ToString();
  EXPECT_EQ(*r, *again);
  EXPECT_EQ(once, again->Serialize());
}

TEST(PluginManifestTest, RejectsInvalid) {
  const auto code = [](const char* body) {
    std::string doc = R"({"kind":"PluginManifest","schema_version":1,)";
    doc += body;
    doc += "}";
    return ge::PluginManifest::ParseJson(doc).status().code();
  };
  const char* base = R"("plugin_id":"p","library":"a.so","abi":{"major":1,"minor":0},"build_fingerprint":"f","operators":["X@1.0.0"],"no_owned_threads":true)";
  {
    std::string doc = R"({"kind":"PluginManifest","schema_version":1,)";
    doc += base;
    doc += "}";
    EXPECT_TRUE(ge::PluginManifest::ParseJson(doc).ok());
  }
  EXPECT_EQ(code(R"("plugin_id":"p","library":"../a.so","abi":{"major":1,"minor":0},"build_fingerprint":"f","operators":["X@1.0.0"],"no_owned_threads":true)"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("plugin_id":"p","library":"/abs/a.so","abi":{"major":1,"minor":0},"build_fingerprint":"f","operators":["X@1.0.0"],"no_owned_threads":true)"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("plugin_id":"p","library":"a.so","abi":{"major":1},"build_fingerprint":"f","operators":["X@1.0.0"],"no_owned_threads":true)"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("plugin_id":"p","library":"a.so","abi":{"major":1,"minor":0},"build_fingerprint":"f","operators":[],"no_owned_threads":true)"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("plugin_id":"p","library":"a.so","abi":{"major":1,"minor":0},"build_fingerprint":"f","operators":["NoVersion"],"no_owned_threads":true)"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("plugin_id":"p","library":"a.so","abi":{"major":1,"minor":0},"build_fingerprint":"f","operators":["X@1.0.0","X@1.0.0"],"no_owned_threads":true)"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("plugin_id":"p","library":"a.so","abi":{"major":1,"minor":0},"build_fingerprint":"f","operators":["X@1.0.0"],"no_owned_threads":false)"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("plugin_id":"p","library":"a.so","abi":{"major":1,"minor":0},"build_fingerprint":"f","operators":["X@1.0.0"])"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("plugin_id":"p","library":"a.so","abi":{"major":1,"minor":0},"build_fingerprint":"f","operators":["X@1.0.0"],"no_owned_threads":true,"resources":["../x"])"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(code(R"("plugin_id":"p","library":"a.so","abi":{"major":1,"minor":0},"build_fingerprint":"f","operators":["X@1.0.0"],"no_owned_threads":true,"typo":1)"),
            GE_STATUS_PLUGIN_MANIFEST_INVALID);
  EXPECT_EQ(ge::PluginManifest::ParseJson("{").status().code(), GE_STATUS_PLUGIN_MANIFEST_INVALID);
}

TEST(PluginManifestTest, SafeRelativePath) {
  EXPECT_TRUE(ge::IsSafeRelativePath("a.so"));
  EXPECT_TRUE(ge::IsSafeRelativePath("models/x.plan"));
  EXPECT_FALSE(ge::IsSafeRelativePath(""));
  EXPECT_FALSE(ge::IsSafeRelativePath("/a"));
  EXPECT_FALSE(ge::IsSafeRelativePath("./a"));
  EXPECT_FALSE(ge::IsSafeRelativePath("a/../b"));
  EXPECT_FALSE(ge::IsSafeRelativePath("a//b"));
  EXPECT_FALSE(ge::IsSafeRelativePath("C:\\x"));
}

}  // namespace
