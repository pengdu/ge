#ifndef GE_CPP_GRAPH_TEMPLATE_H_
#define GE_CPP_GRAPH_TEMPLATE_H_

// GM-3 / OD-10 / TD-01: graph template + parameter instantiation. A
// template is a complete GraphSpec skeleton whose node options may contain
// "${param}" placeholders, plus the declaration of those parameters.
// Instantiate() produces an ordinary GraphSpec per parameter set (one batch
// item, one slice of a long input, ...). Topology, operator keys, node and
// edge ids are fixed by the skeleton -- only options vary -- so validation
// and capability negotiation depend solely on the skeleton and can be done
// once: Prevalidate() caches the ValidatedGraph and every instance created
// through Engine::CreateSession(template, ...) skips A4/A5 (resource
// admission A6 still runs per instance, options feed the estimators).
//
// Substitution rules (node options only, recursive through objects/arrays):
//  * a string that is exactly one placeholder ("${input}") is replaced by
//    the argument value verbatim (any JSON type);
//  * placeholders embedded in a longer string ("${dir}/out_${n}.mp4") are
//    replaced by the argument's text (string/integer/number/bool only);
//  * every placeholder must be declared, every declared parameter must be
//    used, arguments must not name undeclared parameters, and required
//    parameters must be supplied (typo safety, 12 §11 spirit).

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ge/cpp/graph_spec.h>
#include <ge/cpp/graph_validator.h>
#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge {

struct TemplateParameter {
  std::string name;  // stable id, referenced as ${name}
  bool required = true;
  JsonValue default_value;  // used when !required and the argument is absent
  std::string description;
};

class GraphTemplate final {
 public:
  // Checks the declaration against the skeleton's placeholders (both ways).
  [[nodiscard]] static Result<GraphTemplate> Create(GraphSpec skeleton,
                                                    std::vector<TemplateParameter> parameters);

  [[nodiscard]] const GraphSpec& skeleton() const noexcept { return skeleton_; }
  [[nodiscard]] const std::vector<TemplateParameter>& parameters() const noexcept { return parameters_; }

  // |arguments| is a JSON object {param: value}. |instance_name| (when not
  // empty) is appended to the graph name as "<name>#<instance_name>".
  [[nodiscard]] Result<GraphSpec> Instantiate(const JsonValue& arguments,
                                              std::string_view instance_name = {}) const;

  // Validates the skeleton once (A4 + A5); afterwards validated() feeds
  // SessionOptions::prevalidated. Not thread safe against Instantiate --
  // call it before the batch starts (Engine::PrevalidateTemplate,
  // BatchRunner's constructor).
  [[nodiscard]] Status Prevalidate(const CapabilityResolver& resolver);
  [[nodiscard]] std::shared_ptr<const ValidatedGraph> validated() const noexcept { return validated_; }

 private:
  GraphTemplate(GraphSpec skeleton, std::vector<TemplateParameter> parameters)
      : skeleton_(std::move(skeleton)), parameters_(std::move(parameters)) {}

  GraphSpec skeleton_;
  std::vector<TemplateParameter> parameters_;
  std::shared_ptr<const ValidatedGraph> validated_;
};

}  // namespace ge

#endif
