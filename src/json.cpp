#include "ge/cpp/json.h"

#include <cstdio>

namespace ge {
namespace {

class Parser final {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  JsonParseResult Run() {
    JsonParseResult result;
    std::optional<JsonValue> value = ParseValue();
    if (!value.has_value()) {
      result.error = error_.empty() ? "invalid json" : error_;
      return result;
    }
    SkipWhitespace();
    if (position_ != text_.size()) {
      result.error =
          "trailing characters at offset " + std::to_string(position_);
      return result;
    }
    result.value = std::move(value);
    return result;
  }

 private:
  void SkipWhitespace() {
    while (position_ < text_.size() &&
           (text_[position_] == ' ' || text_[position_] == '\n' ||
            text_[position_] == '\r' || text_[position_] == '\t')) {
      ++position_;
    }
  }

  bool Fail(std::string message) {
    if (error_.empty()) {
      error_ = std::move(message) + " at offset " + std::to_string(position_);
    }
    return false;
  }

  bool Consume(std::string_view literal) {
    if (text_.substr(position_, literal.size()) != literal) {
      return Fail("unexpected token");
    }
    position_ += literal.size();
    return true;
  }

  std::optional<JsonValue> ParseValue() {
    if (++depth_ > kMaxDepth) {
      Fail("nesting too deep");
      return std::nullopt;
    }
    std::optional<JsonValue> value = ParseValueInner();
    --depth_;
    return value;
  }

  std::optional<JsonValue> ParseValueInner() {
    SkipWhitespace();
    if (position_ >= text_.size()) {
      Fail("unexpected end of input");
      return std::nullopt;
    }
    const char c = text_[position_];
    if (c == '{') return ParseObject();
    if (c == '[') return ParseArray();
    if (c == '"') {
      std::optional<std::string> s = ParseString();
      if (!s.has_value()) return std::nullopt;
      return JsonValue(std::move(*s));
    }
    if (c == 't') {
      return Consume("true") ? std::optional<JsonValue>(JsonValue(true))
                             : std::nullopt;
    }
    if (c == 'f') {
      return Consume("false") ? std::optional<JsonValue>(JsonValue(false))
                              : std::nullopt;
    }
    if (c == 'n') {
      return Consume("null") ? std::optional<JsonValue>(JsonValue())
                             : std::nullopt;
    }
    return ParseNumber();
  }

  std::optional<JsonValue> ParseNumber() {
    const std::size_t start = position_;
    bool is_integer = true;
    if (position_ < text_.size() && text_[position_] == '-') ++position_;
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c >= '0' && c <= '9') {
        ++position_;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        is_integer = false;
        ++position_;
      } else {
        break;
      }
    }
    if (start == position_) {
      Fail("invalid number");
      return std::nullopt;
    }
    const std::string token(text_.substr(start, position_ - start));
    char* end = nullptr;
    if (is_integer) {
      const long long value = std::strtoll(token.c_str(), &end, 10);
      if (end != nullptr && *end == '\0') {
        return JsonValue(static_cast<std::int64_t>(value));
      }
    }
    const double value = std::strtod(token.c_str(), &end);
    if (end == nullptr || *end != '\0') {
      Fail("invalid number");
      return std::nullopt;
    }
    return JsonValue(value);
  }

  std::optional<std::string> ParseString() {
    if (!Consume("\"")) return std::nullopt;
    std::string out;
    while (position_ < text_.size()) {
      const char c = text_[position_++];
      if (c == '"') return out;
      if (c == '\\') {
        if (position_ >= text_.size()) break;
        const char escaped = text_[position_++];
        switch (escaped) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            if (position_ + 4 > text_.size()) {
              Fail("invalid unicode escape");
              return std::nullopt;
            }
            const std::string hex(text_.substr(position_, 4));
            position_ += 4;
            char* end = nullptr;
            const unsigned long code = std::strtoul(hex.c_str(), &end, 16);
            if (end == nullptr || *end != '\0') {
              Fail("invalid unicode escape");
              return std::nullopt;
            }
            AppendUtf8(out, static_cast<unsigned>(code));
            break;
          }
          default:
            Fail("invalid escape");
            return std::nullopt;
        }
        continue;
      }
      out.push_back(c);
    }
    Fail("unterminated string");
    return std::nullopt;
  }

  static void AppendUtf8(std::string& out, unsigned code) {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  std::optional<JsonValue> ParseArray() {
    if (!Consume("[")) return std::nullopt;
    JsonArray items;
    SkipWhitespace();
    if (position_ < text_.size() && text_[position_] == ']') {
      ++position_;
      return JsonValue(std::move(items));
    }
    while (true) {
      std::optional<JsonValue> item = ParseValue();
      if (!item.has_value()) return std::nullopt;
      items.push_back(std::move(*item));
      SkipWhitespace();
      if (position_ >= text_.size()) {
        Fail("unterminated array");
        return std::nullopt;
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == ']') {
        ++position_;
        return JsonValue(std::move(items));
      }
      Fail("expected ',' or ']'");
      return std::nullopt;
    }
  }

  std::optional<JsonValue> ParseObject() {
    if (!Consume("{")) return std::nullopt;
    JsonObject members;
    SkipWhitespace();
    if (position_ < text_.size() && text_[position_] == '}') {
      ++position_;
      return JsonValue(std::move(members));
    }
    while (true) {
      SkipWhitespace();
      if (position_ >= text_.size() || text_[position_] != '"') {
        Fail("expected string key");
        return std::nullopt;
      }
      std::optional<std::string> key = ParseString();
      if (!key.has_value()) return std::nullopt;
      SkipWhitespace();
      if (!Consume(":")) return std::nullopt;
      std::optional<JsonValue> value = ParseValue();
      if (!value.has_value()) return std::nullopt;
      if (!members.emplace(std::move(*key), std::move(*value)).second) {
        Fail("duplicate key");
        return std::nullopt;
      }
      SkipWhitespace();
      if (position_ >= text_.size()) {
        Fail("unterminated object");
        return std::nullopt;
      }
      if (text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (text_[position_] == '}') {
        ++position_;
        return JsonValue(std::move(members));
      }
      Fail("expected ',' or '}'");
      return std::nullopt;
    }
  }

  static constexpr int kMaxDepth = 256;

  std::string_view text_;
  std::size_t position_ = 0;
  std::string error_;
  int depth_ = 0;
};

}  // namespace

JsonParseResult ParseJson(std::string_view text) { return Parser(text).Run(); }

}  // namespace ge
