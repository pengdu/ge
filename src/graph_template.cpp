#include <ge/cpp/graph_template.h>

#include <set>

namespace ge {

namespace {

// Appends every ${name} found in |text| to |out|.
void CollectPlaceholders(const std::string& text, std::set<std::string>* out) {
  std::size_t pos = 0;
  while ((pos = text.find("${", pos)) != std::string::npos) {
    const std::size_t end = text.find('}', pos + 2);
    if (end == std::string::npos) return;
    out->insert(text.substr(pos + 2, end - pos - 2));
    pos = end + 1;
  }
}

void CollectPlaceholders(const JsonValue& v, std::set<std::string>* out) {
  if (v.is_string()) {
    CollectPlaceholders(v.as_string(), out);
  } else if (v.is_array()) {
    for (const JsonValue& item : v.as_array()) CollectPlaceholders(item, out);
  } else if (v.is_object()) {
    for (const auto& [k, item] : v.as_object()) CollectPlaceholders(item, out);
  }
}

[[nodiscard]] Result<std::string> AsText(const JsonValue& v, const std::string& name) {
  switch (v.type()) {
    case JsonValue::Type::kString: return v.as_string();
    case JsonValue::Type::kInteger: return std::to_string(v.as_integer());
    case JsonValue::Type::kBool: return std::string(v.as_bool() ? "true" : "false");
    case JsonValue::Type::kNumber: {
      std::string s = JsonValue(v.as_number()).Serialize();
      return s;
    }
    default:
      return Status::InvalidArgument("parameter '" + name +
                                     "' is embedded in a string and must be a scalar, got " + v.Serialize());
  }
}

class Substituter {
 public:
  explicit Substituter(const JsonObject& args) : args_(args) {}

  [[nodiscard]] Result<JsonValue> Apply(const JsonValue& v) {
    if (v.is_string()) return ApplyString(v.as_string());
    if (v.is_array()) {
      JsonArray out;
      out.reserve(v.as_array().size());
      for (const JsonValue& item : v.as_array()) {
        Result<JsonValue> r = Apply(item);
        if (!r.ok()) return r;
        out.push_back(std::move(*r));
      }
      return JsonValue(std::move(out));
    }
    if (v.is_object()) {
      JsonObject out;
      for (const auto& [k, item] : v.as_object()) {
        Result<JsonValue> r = Apply(item);
        if (!r.ok()) return r;
        out.emplace(k, std::move(*r));
      }
      return JsonValue(std::move(out));
    }
    return v;
  }

 private:
  [[nodiscard]] Result<JsonValue> ApplyString(const std::string& text) {
    // Whole-string placeholder: verbatim value of any type.
    if (text.size() > 3 && text.starts_with("${") && text.ends_with('}') &&
        text.find("${", 2) == std::string::npos && text.find('}') == text.size() - 1) {
      return Lookup(text.substr(2, text.size() - 3));
    }
    std::string out;
    std::size_t pos = 0;
    for (;;) {
      const std::size_t open = text.find("${", pos);
      if (open == std::string::npos) {
        out.append(text, pos, std::string::npos);
        break;
      }
      const std::size_t close = text.find('}', open + 2);
      if (close == std::string::npos) {
        out.append(text, pos, std::string::npos);
        break;
      }
      out.append(text, pos, open - pos);
      const std::string name = text.substr(open + 2, close - open - 2);
      Result<JsonValue> value = Lookup(name);
      if (!value.ok()) return value.status();
      Result<std::string> piece = AsText(*value, name);
      if (!piece.ok()) return piece.status();
      out.append(*piece);
      pos = close + 1;
    }
    return JsonValue(std::move(out));
  }

  [[nodiscard]] Result<JsonValue> Lookup(const std::string& name) {
    const auto it = args_.find(name);
    if (it == args_.end()) return Status::InvalidArgument("no value for template parameter '" + name + "'");
    return it->second;
  }

  const JsonObject& args_;
};

}  // namespace

Result<GraphTemplate> GraphTemplate::Create(GraphSpec skeleton, std::vector<TemplateParameter> parameters) {
  std::set<std::string> used;
  for (const NodeSpec& n : skeleton.nodes()) CollectPlaceholders(n.options, &used);
  std::set<std::string> declared;
  for (const TemplateParameter& p : parameters) {
    if (p.name.empty()) return Status::InvalidArgument("template parameter with empty name");
    if (!declared.insert(p.name).second) {
      return Status::InvalidArgument("template parameter '" + p.name + "' declared twice");
    }
    if (!p.required && p.default_value.is_null()) {
      return Status::InvalidArgument("optional template parameter '" + p.name + "' needs a default value");
    }
  }
  for (const std::string& name : used) {
    if (!declared.contains(name)) {
      return Status::InvalidArgument("template uses undeclared parameter '" + name + "'");
    }
  }
  for (const std::string& name : declared) {
    if (!used.contains(name)) {
      return Status::InvalidArgument("template declares unused parameter '" + name + "'");
    }
  }
  return GraphTemplate(std::move(skeleton), std::move(parameters));
}

Result<GraphSpec> GraphTemplate::Instantiate(const JsonValue& arguments, std::string_view instance_name) const {
  if (!arguments.is_object()) return Status::InvalidArgument("template arguments must be a JSON object");
  JsonObject args;
  for (const auto& [k, v] : arguments.as_object()) {
    bool known = false;
    for (const TemplateParameter& p : parameters_) known = known || p.name == k;
    if (!known) return Status::InvalidArgument("argument '" + k + "' does not name a template parameter");
    args.emplace(k, v);
  }
  for (const TemplateParameter& p : parameters_) {
    if (args.contains(p.name)) continue;
    if (p.required) return Status::InvalidArgument("missing required template parameter '" + p.name + "'");
    args.emplace(p.name, p.default_value);
  }
  GraphSpec spec = skeleton_;
  if (!instance_name.empty()) spec.set_name(spec.name() + "#" + std::string(instance_name));
  Substituter sub(args);
  for (const NodeSpec& n : skeleton_.nodes()) {
    Result<JsonValue> r = sub.Apply(n.options);
    if (!r.ok()) return Status::InvalidArgument("node '" + n.id + "': " + r.status().message());
    spec.FindNode(n.id)->options = std::move(*r);
  }
  return spec;
}

Status GraphTemplate::Prevalidate(const CapabilityResolver& resolver, std::uint64_t operator_generation) {
  GraphValidator validator(resolver);
  Result<ValidatedGraph> validated = validator.Validate(skeleton_);
  if (!validated.ok()) return validated.status();
  validated_ = std::make_shared<const ValidatedGraph>(std::move(*validated));
  validated_generation_ = operator_generation;
  return Status::Ok();
}

}  // namespace ge
