#include <ge/cpp/plugin_manifest.h>

#include <set>

#include "json_reader.h"

namespace ge {

using internal::ObjectReader;

bool IsSafeRelativePath(std::string_view path) noexcept {
  if (path.empty() || path.front() == '/' || path.front() == '\\') return false;
  if (path.size() >= 2 && path[1] == ':') return false;  // drive letter
  std::size_t start = 0;
  while (start <= path.size()) {
    std::size_t end = path.find_first_of("/\\", start);
    if (end == std::string_view::npos) end = path.size();
    const std::string_view part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return false;
    start = end + 1;
  }
  return true;
}

Result<PluginManifest> PluginManifest::ParseJson(std::string_view json) {
  JsonParseResult parsed = ge::ParseJson(json);
  if (!parsed.ok()) {
    return Status::PluginManifestInvalid("invalid manifest JSON: " + parsed.error);
  }
  return ParseJson(*parsed.value);
}

Result<PluginManifest> PluginManifest::ParseJson(const JsonValue& doc) {
  bool allow_unknown = false;
  if (Status s = internal::CheckDocumentHeader(doc, "PluginManifest", kSchemaVersion, kSchemaId,
                                               &allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
      !s.ok()) {
    return s;
  }
  ObjectReader r(doc, "PluginManifest", allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
  internal::MarkHeaderSeen(r);
  PluginManifest m;
  m.allow_unknown_fields = allow_unknown;

  if (Status s = r.RequireString("plugin_id", &m.plugin_id); !s.ok()) return s;
  if (m.plugin_id.empty()) return r.Error("plugin_id must not be empty");
  if (Status s = r.RequireString("library", &m.library); !s.ok()) return s;
  if (!IsSafeRelativePath(m.library)) {
    return r.Error("library '" + m.library + "' must be a safe relative path");
  }

  const JsonValue* abi = r.Get("abi");
  if (abi == nullptr) return r.Missing("abi");
  {
    ObjectReader ar(*abi, "abi", allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
    if (!ar.ok()) return r.WrongType("abi", "object");
    std::int64_t major = 0, minor = 0;
    if (Status s = ar.RequireInteger("major", &major); !s.ok()) return s;
    if (Status s = ar.RequireInteger("minor", &minor); !s.ok()) return s;
    if (major < 0 || minor < 0) return ar.Error("abi versions must be non-negative");
    m.abi.major = static_cast<std::uint32_t>(major);
    m.abi.minor = static_cast<std::uint32_t>(minor);
    if (Status s = ar.Finish(); !s.ok()) return s;
  }

  if (Status s = r.RequireString("build_fingerprint", &m.build_fingerprint); !s.ok()) return s;
  if (m.build_fingerprint.empty()) return r.Error("build_fingerprint must not be empty");

  if (const JsonValue* deps = r.Get("dependencies"); deps != nullptr) {
    if (!deps->is_array()) return r.WrongType("dependencies", "array");
    std::size_t i = 0;
    for (const JsonValue& d : deps->as_array()) {
      ObjectReader dr(d, "dependencies[" + std::to_string(i++) + "]", allow_unknown,
                      GE_STATUS_PLUGIN_MANIFEST_INVALID);
      if (!dr.ok()) return r.Error(dr.path() + " must be an object");
      PluginDependency dep;
      if (Status s = dr.RequireString("name", &dep.name); !s.ok()) return s;
      if (Status s = dr.RequireString("version", &dep.version); !s.ok()) return s;
      if (Status s = dr.Finish(); !s.ok()) return s;
      m.dependencies.push_back(std::move(dep));
    }
  }

  if (Status s = r.StringArray("resources", false, &m.resources); !s.ok()) return s;
  for (const std::string& res : m.resources) {
    if (!IsSafeRelativePath(res)) {
      return r.Error("resource '" + res + "' must be a safe relative path");
    }
  }

  std::vector<std::string> ops;
  if (Status s = r.StringArray("operators", true, &ops); !s.ok()) return s;
  if (ops.empty()) return r.Error("operators must not be empty");
  std::set<std::string> seen;
  for (const std::string& op : ops) {
    auto key = OperatorKey::Parse(op);
    if (!key) return r.Error("invalid operator key '" + op + "'");
    if (!seen.insert(op).second) return r.Error("duplicate operator '" + op + "'");
    m.operators.push_back(std::move(*key));
  }

  std::optional<std::int64_t> mi;
  if (Status s = r.OptionalInteger("max_inference_ms", &mi); !s.ok()) return s;
  if (mi) {
    if (*mi <= 0) return r.Error("max_inference_ms must be > 0");
    m.max_inference_ms = static_cast<std::uint64_t>(*mi);
  }

  if (Status s = r.RequireBool("no_owned_threads", &m.no_owned_threads); !s.ok()) return s;
  if (!m.no_owned_threads) return r.Error("no_owned_threads must be true");

  if (Status s = r.Finish(); !s.ok()) return s;
  return m;
}

JsonValue PluginManifest::ToJson() const {
  JsonObject o;
  o.emplace("kind", JsonValue("PluginManifest"));
  o.emplace("schema_version", JsonValue(static_cast<std::int64_t>(kSchemaVersion)));
  o.emplace("$id", JsonValue(kSchemaId));
  if (allow_unknown_fields) o.emplace("allow_unknown_fields", JsonValue(true));
  o.emplace("plugin_id", JsonValue(plugin_id));
  o.emplace("library", JsonValue(library));
  JsonObject abi_obj;
  abi_obj.emplace("major", JsonValue(static_cast<std::int64_t>(abi.major)));
  abi_obj.emplace("minor", JsonValue(static_cast<std::int64_t>(abi.minor)));
  o.emplace("abi", JsonValue(std::move(abi_obj)));
  o.emplace("build_fingerprint", JsonValue(build_fingerprint));
  if (!dependencies.empty()) {
    JsonArray deps;
    for (const PluginDependency& d : dependencies) {
      JsonObject dd;
      dd.emplace("name", JsonValue(d.name));
      dd.emplace("version", JsonValue(d.version));
      deps.push_back(JsonValue(std::move(dd)));
    }
    o.emplace("dependencies", JsonValue(std::move(deps)));
  }
  if (!resources.empty()) {
    JsonArray res;
    for (const std::string& p : resources) res.push_back(JsonValue(p));
    o.emplace("resources", JsonValue(std::move(res)));
  }
  JsonArray ops;
  for (const OperatorKey& k : operators) ops.push_back(JsonValue(k.ToString()));
  o.emplace("operators", JsonValue(std::move(ops)));
  if (max_inference_ms) {
    o.emplace("max_inference_ms", JsonValue(static_cast<std::int64_t>(*max_inference_ms)));
  }
  o.emplace("no_owned_threads", JsonValue(no_owned_threads));
  return JsonValue(std::move(o));
}

}  // namespace ge
