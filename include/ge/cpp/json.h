#ifndef GE_CPP_JSON_H_
#define GE_CPP_JSON_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ge {

class JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

class JsonValue final {
 public:
  enum class Type { kNull, kBool, kInteger, kNumber, kString, kArray, kObject };

  JsonValue() = default;
  explicit JsonValue(bool value) : data_(value) {}
  explicit JsonValue(std::int64_t value) : data_(value) {}
  explicit JsonValue(int value) : data_(static_cast<std::int64_t>(value)) {}
  explicit JsonValue(std::uint64_t value)
      : data_(static_cast<std::int64_t>(value)) {}
  explicit JsonValue(double value) : data_(value) {}
  explicit JsonValue(std::string value) : data_(std::move(value)) {}
  explicit JsonValue(const char* value) : data_(std::string(value)) {}
  explicit JsonValue(std::string_view value) : data_(std::string(value)) {}
  explicit JsonValue(JsonArray value)
      : data_(std::make_shared<JsonArray>(std::move(value))) {}
  explicit JsonValue(JsonObject value)
      : data_(std::make_shared<JsonObject>(std::move(value))) {}

  [[nodiscard]] Type type() const noexcept {
    return static_cast<Type>(data_.index());
  }
  [[nodiscard]] bool is_null() const noexcept { return type() == Type::kNull; }
  [[nodiscard]] bool is_bool() const noexcept { return type() == Type::kBool; }
  [[nodiscard]] bool is_integer() const noexcept {
    return type() == Type::kInteger;
  }
  [[nodiscard]] bool is_number() const noexcept {
    return type() == Type::kNumber || type() == Type::kInteger;
  }
  [[nodiscard]] bool is_string() const noexcept {
    return type() == Type::kString;
  }
  [[nodiscard]] bool is_array() const noexcept { return type() == Type::kArray; }
  [[nodiscard]] bool is_object() const noexcept {
    return type() == Type::kObject;
  }

  [[nodiscard]] bool as_bool() const { return std::get<bool>(data_); }
  [[nodiscard]] std::int64_t as_integer() const {
    if (type() == Type::kNumber) {
      return static_cast<std::int64_t>(std::get<double>(data_));
    }
    return std::get<std::int64_t>(data_);
  }
  [[nodiscard]] double as_number() const {
    if (type() == Type::kInteger) {
      return static_cast<double>(std::get<std::int64_t>(data_));
    }
    return std::get<double>(data_);
  }
  [[nodiscard]] const std::string& as_string() const {
    return std::get<std::string>(data_);
  }
  [[nodiscard]] const JsonArray& as_array() const {
    return *std::get<std::shared_ptr<JsonArray>>(data_);
  }
  [[nodiscard]] const JsonObject& as_object() const {
    return *std::get<std::shared_ptr<JsonObject>>(data_);
  }
  [[nodiscard]] JsonArray& mutable_array() {
    return *std::get<std::shared_ptr<JsonArray>>(data_);
  }
  [[nodiscard]] JsonObject& mutable_object() {
    return *std::get<std::shared_ptr<JsonObject>>(data_);
  }

  [[nodiscard]] const JsonValue* Find(std::string_view key) const {
    if (!is_object()) {
      return nullptr;
    }
    const auto& object = as_object();
    const auto iterator = object.find(std::string(key));
    return iterator == object.end() ? nullptr : &iterator->second;
  }

  [[nodiscard]] std::optional<std::string> GetString(
      std::string_view key) const {
    const JsonValue* v = Find(key);
    if (v == nullptr || !v->is_string()) {
      return std::nullopt;
    }
    return v->as_string();
  }
  [[nodiscard]] std::optional<std::int64_t> GetInteger(
      std::string_view key) const {
    const JsonValue* v = Find(key);
    if (v == nullptr || !v->is_number()) {
      return std::nullopt;
    }
    return v->as_integer();
  }
  [[nodiscard]] std::optional<bool> GetBool(std::string_view key) const {
    const JsonValue* v = Find(key);
    if (v == nullptr || !v->is_bool()) {
      return std::nullopt;
    }
    return v->as_bool();
  }

  [[nodiscard]] bool operator==(const JsonValue& other) const {
    if (type() != other.type()) {
      if (is_number() && other.is_number()) {
        return as_number() == other.as_number();
      }
      return false;
    }
    switch (type()) {
      case Type::kNull: return true;
      case Type::kBool: return as_bool() == other.as_bool();
      case Type::kInteger: return as_integer() == other.as_integer();
      case Type::kNumber: return as_number() == other.as_number();
      case Type::kString: return as_string() == other.as_string();
      case Type::kArray: return as_array() == other.as_array();
      case Type::kObject: return as_object() == other.as_object();
    }
    return false;
  }

  // Deterministic, compact serialization: object keys sorted (std::map order),
  // no whitespace. Two equal values always serialize to identical bytes.
  [[nodiscard]] std::string Serialize() const {
    std::string out;
    SerializeTo(out);
    return out;
  }

  void SerializeTo(std::string& out) const {
    switch (type()) {
      case Type::kNull:
        out += "null";
        return;
      case Type::kBool:
        out += as_bool() ? "true" : "false";
        return;
      case Type::kInteger:
        out += std::to_string(as_integer());
        return;
      case Type::kNumber: {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.17g", as_number());
        out += buffer;
        return;
      }
      case Type::kString:
        AppendQuoted(out, as_string());
        return;
      case Type::kArray: {
        out.push_back('[');
        bool first = true;
        for (const JsonValue& item : as_array()) {
          if (!first) out.push_back(',');
          first = false;
          item.SerializeTo(out);
        }
        out.push_back(']');
        return;
      }
      case Type::kObject: {
        out.push_back('{');
        bool first = true;
        for (const auto& [key, value] : as_object()) {
          if (!first) out.push_back(',');
          first = false;
          AppendQuoted(out, key);
          out.push_back(':');
          value.SerializeTo(out);
        }
        out.push_back('}');
        return;
      }
    }
  }

 private:
  static void AppendQuoted(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                          static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += buffer;
          } else {
            out.push_back(c);
          }
      }
    }
    out.push_back('"');
  }

  std::variant<std::monostate, bool, std::int64_t, double, std::string,
               std::shared_ptr<JsonArray>, std::shared_ptr<JsonObject>>
      data_;
};

struct JsonParseResult {
  std::optional<JsonValue> value;
  std::string error;
  [[nodiscard]] bool ok() const noexcept { return value.has_value(); }
};

[[nodiscard]] JsonParseResult ParseJson(std::string_view text);

}  // namespace ge

#endif
