#ifndef GE_SRC_JSON_READER_H_
#define GE_SRC_JSON_READER_H_

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "ge/cpp/json.h"
#include "ge/cpp/types.h"

namespace ge::internal {

// Field-level reader that tracks consumed keys so unknown fields can be
// rejected deterministically (14 §1.1).
class ObjectReader final {
 public:
  ObjectReader(const JsonValue& value, std::string path, bool allow_unknown,
               ge_status_code error_code = GE_STATUS_GRAPH_INVALID)
      : value_(value),
        path_(std::move(path)),
        allow_unknown_(allow_unknown),
        error_code_(error_code) {}

  [[nodiscard]] bool ok() const noexcept { return value_.is_object(); }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  [[nodiscard]] const JsonValue* Get(std::string_view key) {
    seen_.emplace(key);
    return value_.Find(key);
  }

  [[nodiscard]] Status RequireString(std::string_view key, std::string* out) {
    const JsonValue* v = Get(key);
    if (v == nullptr) return Missing(key);
    if (!v->is_string()) return WrongType(key, "string");
    *out = v->as_string();
    return Status::Ok();
  }
  [[nodiscard]] Status OptionalString(std::string_view key, std::string* out) {
    const JsonValue* v = Get(key);
    if (v == nullptr) return Status::Ok();
    if (!v->is_string()) return WrongType(key, "string");
    *out = v->as_string();
    return Status::Ok();
  }
  [[nodiscard]] Status OptionalInteger(std::string_view key,
                                       std::optional<std::int64_t>* out) {
    const JsonValue* v = Get(key);
    if (v == nullptr) return Status::Ok();
    if (!v->is_integer()) return WrongType(key, "integer");
    *out = v->as_integer();
    return Status::Ok();
  }
  [[nodiscard]] Status RequireInteger(std::string_view key, std::int64_t* out) {
    const JsonValue* v = Get(key);
    if (v == nullptr) return Missing(key);
    if (!v->is_integer()) return WrongType(key, "integer");
    *out = v->as_integer();
    return Status::Ok();
  }
  [[nodiscard]] Status OptionalNumber(std::string_view key,
                                      std::optional<double>* out) {
    const JsonValue* v = Get(key);
    if (v == nullptr) return Status::Ok();
    if (!v->is_number()) return WrongType(key, "number");
    *out = v->as_number();
    return Status::Ok();
  }
  [[nodiscard]] Status OptionalBool(std::string_view key, bool* out) {
    const JsonValue* v = Get(key);
    if (v == nullptr) return Status::Ok();
    if (!v->is_bool()) return WrongType(key, "boolean");
    *out = v->as_bool();
    return Status::Ok();
  }
  [[nodiscard]] Status RequireBool(std::string_view key, bool* out) {
    const JsonValue* v = Get(key);
    if (v == nullptr) return Missing(key);
    if (!v->is_bool()) return WrongType(key, "boolean");
    *out = v->as_bool();
    return Status::Ok();
  }
  [[nodiscard]] Status OptionalObject(std::string_view key, JsonValue* out) {
    const JsonValue* v = Get(key);
    if (v == nullptr) return Status::Ok();
    if (!v->is_object()) return WrongType(key, "object");
    *out = *v;
    return Status::Ok();
  }
  [[nodiscard]] Status OptionalStringArray(std::string_view key,
                                           std::optional<std::vector<std::string>>* out) {
    const JsonValue* v = Get(key);
    if (v == nullptr) return Status::Ok();
    if (!v->is_array()) return WrongType(key, "array");
    std::vector<std::string> items;
    for (const JsonValue& item : v->as_array()) {
      if (!item.is_string()) {
        return Error("elements of '" + std::string(key) + "' in " + path_ +
                     " must be strings");
      }
      items.push_back(item.as_string());
    }
    *out = std::move(items);
    return Status::Ok();
  }
  [[nodiscard]] Status StringArray(std::string_view key, bool required,
                                   std::vector<std::string>* out) {
    std::optional<std::vector<std::string>> tmp;
    if (Status s = OptionalStringArray(key, &tmp); !s.ok()) return s;
    if (!tmp) return required ? Missing(key) : Status::Ok();
    *out = std::move(*tmp);
    return Status::Ok();
  }

  [[nodiscard]] Status Finish() const {
    if (allow_unknown_) return Status::Ok();
    for (const auto& [key, _] : value_.as_object()) {
      if (!seen_.contains(key)) {
        return Error("unknown field '" + key + "' in " + path_);
      }
    }
    return Status::Ok();
  }

  [[nodiscard]] Status Error(std::string message) const {
    return Status(error_code_, std::move(message));
  }
  [[nodiscard]] Status Missing(std::string_view key) const {
    return Error("missing field '" + std::string(key) + "' in " + path_);
  }
  [[nodiscard]] Status WrongType(std::string_view key,
                                 std::string_view expected) const {
    return Error("field '" + std::string(key) + "' in " + path_ + " must be " +
                 std::string(expected));
  }

 private:
  const JsonValue& value_;
  std::string path_;
  bool allow_unknown_;
  ge_status_code error_code_;
  std::set<std::string, std::less<>> seen_;
};

// 14 §1.1 / 13 §10 document header rules.
inline Status CheckDocumentHeader(const JsonValue& doc, std::string_view kind,
                                  int schema_version, std::string_view schema_id,
                                  bool* allow_unknown,
                                  ge_status_code error_code = GE_STATUS_GRAPH_INVALID) {
  const auto fail = [&](std::string m, std::string ctx = {}) {
    return Status(error_code, std::move(m), false, std::move(ctx));
  };
  if (!doc.is_object()) return fail("document must be a JSON object");
  const auto k = doc.GetString("kind");
  if (!k) return fail("missing document 'kind'");
  if (*k != kind) {
    return fail("expected kind '" + std::string(kind) + "', got '" + *k + "'");
  }
  const JsonValue* sv = doc.Find("schema_version");
  if (sv == nullptr || !sv->is_integer()) {
    return fail("missing or non-integer 'schema_version'");
  }
  if (sv->as_integer() != schema_version) {
    return fail("unsupported schema_version " + std::to_string(sv->as_integer()) +
                    " (expected " + std::to_string(schema_version) + ")",
                "{\"expected\":" + std::to_string(schema_version) +
                    ",\"received\":" + std::to_string(sv->as_integer()) + "}");
  }
  if (const JsonValue* id = doc.Find("$id"); id != nullptr) {
    if (!id->is_string()) return fail("'$id' must be a string");
    if (id->as_string() != schema_id) {
      return fail("'$id' '" + id->as_string() + "' does not match schema_version");
    }
  }
  *allow_unknown = false;
  if (const JsonValue* au = doc.Find("allow_unknown_fields"); au != nullptr) {
    if (!au->is_bool()) return fail("'allow_unknown_fields' must be boolean");
    *allow_unknown = au->as_bool();
  }
  return Status::Ok();
}

inline void MarkHeaderSeen(ObjectReader& r) {
  (void)r.Get("kind");
  (void)r.Get("schema_version");
  (void)r.Get("$id");
  (void)r.Get("allow_unknown_fields");
}

// FNV-1a 64-bit, used for content-addressed versions and cache keys.
inline std::uint64_t Fnv1a(std::string_view data, std::uint64_t seed = 0xcbf29ce484222325ULL) noexcept {
  std::uint64_t h = seed;
  for (const char c : data) {
    h ^= static_cast<unsigned char>(c);
    h *= 0x100000001b3ULL;
  }
  return h;
}

}  // namespace ge::internal

#endif
