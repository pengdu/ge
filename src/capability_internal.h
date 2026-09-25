#ifndef GE_SRC_CAPABILITY_INTERNAL_H_
#define GE_SRC_CAPABILITY_INTERNAL_H_

#include <string>
#include <vector>

#include "ge/cpp/capability.h"
#include "ge/cpp/json.h"

// JSON renderers shared by the two capability translation units: the
// serialization TU (capability_json.cpp) owns the full schema, the
// negotiation TU (capability.cpp) borrows them to render conflict payloads.

namespace ge::internal {

inline JsonValue RangeJson(const IntRange& r) {
  JsonObject o;
  o.emplace("min", JsonValue(r.min));
  o.emplace("max", JsonValue(r.max));
  return JsonValue(std::move(o));
}

inline JsonValue RangeJson(const RationalRange& r) {
  JsonObject o;
  o.emplace("min", JsonValue(r.min));
  o.emplace("max", JsonValue(r.max));
  return JsonValue(std::move(o));
}

inline JsonValue StringsJson(const std::vector<std::string>& v) {
  JsonArray a;
  for (const std::string& s : v) a.push_back(JsonValue(s));
  return JsonValue(std::move(a));
}

}  // namespace ge::internal

#endif
