#ifndef GE_CPP_GRAPH_SPEC_JSON_H_
#define GE_CPP_GRAPH_SPEC_JSON_H_

#include <string>
#include <string_view>

#include <ge/cpp/graph_spec.h>
#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge {

//
//  * kind / schema_version are mandatory; $id major must match;
//  * unknown fields are rejected unless the top level sets
//    allow_unknown_fields=true.
// Serialization is deterministic: parse(serialize(x)) == x and
// serialize(parse(serialize(x))) == serialize(x).
class GraphSpecParser final {
 public:
  [[nodiscard]] static Result<GraphSpec> ParseGraph(std::string_view json);
  [[nodiscard]] static Result<GraphSpec> ParseGraph(const JsonValue& doc);
  [[nodiscard]] static std::string SerializeGraph(const GraphSpec& spec);
  [[nodiscard]] static JsonValue ToJson(const GraphSpec& spec);

  [[nodiscard]] static Result<MutationPatch> ParsePatch(std::string_view json);
  [[nodiscard]] static Result<MutationPatch> ParsePatch(const JsonValue& doc);
  [[nodiscard]] static std::string SerializePatch(const MutationPatch& patch);
  [[nodiscard]] static JsonValue ToJson(const MutationPatch& patch);

  [[nodiscard]] static JsonValue ToJson(const NodeSpec& node);
  [[nodiscard]] static JsonValue ToJson(const EdgeSpec& edge);
};

}  // namespace ge

#endif
