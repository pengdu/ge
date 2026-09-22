#ifndef GE_CPP_PLUGIN_MANIFEST_H_
#define GE_CPP_PLUGIN_MANIFEST_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge {

struct PluginDependency {
  std::string name;
  std::string version;  // constraint text, interpreted by the registry
  friend bool operator==(const PluginDependency&, const PluginDependency&) = default;
};

struct AbiVersion {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  friend bool operator==(const AbiVersion&, const AbiVersion&) = default;
};

struct PluginManifest {
  static constexpr int kSchemaVersion = 1;
  static constexpr std::string_view kSchemaId = "ge.dev/schema/plugin/v1";

  std::string plugin_id;
  std::string library;  // relative to the manifest directory, no traversal
  AbiVersion abi;
  std::string build_fingerprint;
  std::vector<PluginDependency> dependencies;
  std::vector<std::string> resources;  // relative paths, no traversal
  std::vector<OperatorKey> operators;
  std::optional<std::uint64_t> max_inference_ms;
  bool no_owned_threads = true;
  bool allow_unknown_fields = false;

  [[nodiscard]] static Result<PluginManifest> ParseJson(std::string_view json);
  [[nodiscard]] static Result<PluginManifest> ParseJson(const JsonValue& doc);
  [[nodiscard]] JsonValue ToJson() const;
  [[nodiscard]] std::string Serialize() const { return ToJson().Serialize(); }

  friend bool operator==(const PluginManifest&, const PluginManifest&) = default;
};

// Relative path rule shared by |library| and |resources|: non-empty, not
// absolute, no "." / ".." components.
[[nodiscard]] bool IsSafeRelativePath(std::string_view path) noexcept;

}  // namespace ge

#endif
