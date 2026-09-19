// ge_abi_check (PLG-9): dry-run loads plugin manifests through the same
// validation chain the engine uses and prints a JSON verdict per manifest.
// Exit code 0 when every manifest passes (or, with --expect-fail, when
// every manifest is rejected).
//
//   ge_abi_check [--search-path DIR]... [--expect-fail] [--fingerprint]
//                manifest.json...

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <ge/c/ge_plugin.h>
#include <ge/cpp/plugin_registry.h>

int main(int argc, char** argv) {
  std::vector<std::filesystem::path> search;
  std::vector<std::filesystem::path> manifests;
  bool expect_fail = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--search-path" && i + 1 < argc) {
      search.emplace_back(argv[++i]);
    } else if (a == "--expect-fail") {
      expect_fail = true;
    } else if (a == "--fingerprint") {
      std::printf("%s\n", GE_BUILD_FINGERPRINT);
      return 0;
    } else if (a == "--help" || a == "-h") {
      std::printf("usage: ge_abi_check [--search-path DIR]... [--expect-fail] manifest.json...\n");
      return 0;
    } else {
      manifests.emplace_back(a);
    }
  }
  if (manifests.empty()) {
    std::fprintf(stderr, "no manifests given\n");
    return 2;
  }
  if (search.empty()) {
    for (const auto& m : manifests) search.push_back(m.parent_path());
  }
  ge::PluginRegistryOptions options;
  options.search_paths = search;
  ge::PluginRegistry registry(std::move(options));
  int failures = 0;
  std::printf("{\"engine_fingerprint\":\"%s\",\"results\":[", GE_BUILD_FINGERPRINT);
  bool first = true;
  for (const auto& m : manifests) {
    auto r = registry.DryRunLoad(m);
    ge::JsonObject o;
    o["manifest"] = ge::JsonValue(m.string());
    o["ok"] = ge::JsonValue(r.ok());
    if (r.ok()) {
      o["plugin"] = r->ToJson();
    } else {
      o["code"] = ge::JsonValue(ge::Status::CodeName(r.status().code()));
      o["message"] = ge::JsonValue(r.status().message());
      if (!r.status().context_json().empty()) {
        if (auto ctx = ge::ParseJson(r.status().context_json()); ctx.ok()) o["context"] = *ctx.value;
      }
    }
    if (r.ok() == expect_fail) ++failures;
    std::printf("%s%s", first ? "" : ",", ge::JsonValue(std::move(o)).Serialize().c_str());
    first = false;
  }
  std::printf("],\"failures\":%d}\n", failures);
  return failures == 0 ? 0 : 1;
}
