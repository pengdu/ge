#include "ge/cpp/capability.h"

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

#include "capability_internal.h"
#include "json_reader.h"

// CapabilityDescriptor / ConnectionContract / CapabilityConflict JSON
// serialization and parsing (schema 14 §2). Split from capability.cpp so
// schema changes and negotiation-policy changes stop sharing one file;
// negotiation lives in capability.cpp.

namespace ge {

using internal::ObjectReader;
using internal::RangeJson;
using internal::StringsJson;

// ---------------------------------------------------------------------------
// Descriptor: JSON
// ---------------------------------------------------------------------------

namespace {

Status ParseIntRange(ObjectReader& r, std::string_view key, std::optional<IntRange>* out) {
  const JsonValue* v = r.Get(key);
  if (v == nullptr) return Status::Ok();
  ObjectReader rr(*v, r.path() + "." + std::string(key), false, GE_STATUS_PLUGIN_MANIFEST_INVALID);
  if (!rr.ok()) return r.WrongType(key, "object {min,max}");
  std::int64_t min = 0, max = 0;
  if (Status s = rr.RequireInteger("min", &min); !s.ok()) return s;
  if (Status s = rr.RequireInteger("max", &max); !s.ok()) return s;
  if (Status s = rr.Finish(); !s.ok()) return s;
  if (max < min) return r.Error(r.path() + "." + std::string(key) + ": max < min");
  *out = IntRange{min, max};
  return Status::Ok();
}
Status ParseRationalRange(ObjectReader& r, std::string_view key, std::optional<RationalRange>* out) {
  const JsonValue* v = r.Get(key);
  if (v == nullptr) return Status::Ok();
  ObjectReader rr(*v, r.path() + "." + std::string(key), false, GE_STATUS_PLUGIN_MANIFEST_INVALID);
  if (!rr.ok()) return r.WrongType(key, "object {min,max}");
  std::optional<double> min, max;
  if (Status s = rr.OptionalNumber("min", &min); !s.ok()) return s;
  if (Status s = rr.OptionalNumber("max", &max); !s.ok()) return s;
  if (!min || !max) return rr.Missing("min/max");
  if (Status s = rr.Finish(); !s.ok()) return s;
  if (*max < *min) return r.Error(r.path() + "." + std::string(key) + ": max < min");
  *out = RationalRange{*min, *max};
  return Status::Ok();
}

Status ParsePort(const JsonValue& value, const std::string& path, PortDirection dir,
                 bool allow_unknown, PortCapability* out) {
  ObjectReader r(value, path, allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
  if (!r.ok()) return Status::PluginManifestInvalid(path + " must be an object");
  out->direction = dir;
  if (Status s = r.RequireString("name", &out->name); !s.ok()) return s;
  if (Status s = r.RequireString("type_tag", &out->type_tag); !s.ok()) return s;
  if (Status s = r.OptionalBool("required", &out->required); !s.ok()) return s;
  std::string card;
  if (Status s = r.OptionalString("cardinality", &card); !s.ok()) return s;
  if (!card.empty()) {
    if (card == "single") out->cardinality = PortCardinality::kSingle;
    else if (card == "multi") out->cardinality = PortCardinality::kMulti;
    else return r.Error("invalid cardinality '" + card + "' in " + path);
  }
  if (Status s = r.OptionalBool("dynamic_consumers", &out->dynamic_consumers); !s.ok()) return s;

  if (const JsonValue* v = r.Get("video"); v != nullptr) {
    ObjectReader vr(*v, path + ".video", allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
    if (!vr.ok()) return r.WrongType("video", "object");
    VideoConstraints vc;
    if (Status s = vr.OptionalStringArray("pixel_formats", &vc.pixel_formats); !s.ok()) return s;
    if (Status s = ParseIntRange(vr, "width", &vc.width); !s.ok()) return s;
    if (Status s = ParseIntRange(vr, "height", &vc.height); !s.ok()) return s;
    if (Status s = ParseRationalRange(vr, "fps", &vc.fps); !s.ok()) return s;
    if (Status s = vr.OptionalStringArray("color_spaces", &vc.color_spaces); !s.ok()) return s;
    if (Status s = vr.OptionalStringArray("color_ranges", &vc.color_ranges); !s.ok()) return s;
    if (Status s = vr.OptionalStringArray("hdr_modes", &vc.hdr_modes); !s.ok()) return s;
    if (Status s = vr.Finish(); !s.ok()) return s;
    out->video = std::move(vc);
  }
  if (const JsonValue* v = r.Get("audio"); v != nullptr) {
    ObjectReader ar(*v, path + ".audio", allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
    if (!ar.ok()) return r.WrongType("audio", "object");
    AudioConstraints ac;
    if (Status s = ar.OptionalStringArray("sample_formats", &ac.sample_formats); !s.ok()) return s;
    if (Status s = ParseIntRange(ar, "sample_rate", &ac.sample_rate); !s.ok()) return s;
    if (Status s = ar.OptionalStringArray("channel_layouts", &ac.channel_layouts); !s.ok()) return s;
    if (Status s = ar.Finish(); !s.ok()) return s;
    out->audio = std::move(ac);
  }
  if (const JsonValue* v = r.Get("tensor"); v != nullptr) {
    ObjectReader tr(*v, path + ".tensor", allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
    if (!tr.ok()) return r.WrongType("tensor", "object");
    TensorConstraints tc;
    if (Status s = tr.OptionalStringArray("dtypes", &tc.dtypes); !s.ok()) return s;
    if (Status s = tr.OptionalStringArray("layouts", &tc.layouts); !s.ok()) return s;
    if (const JsonValue* shape = tr.Get("shape"); shape != nullptr) {
      if (!shape->is_array()) return tr.WrongType("shape", "array");
      for (const JsonValue& dim : shape->as_array()) {
        if (dim.is_integer()) {
          tc.shape.push_back({dim.as_integer(), dim.as_integer()});
        } else if (dim.is_object()) {
          ObjectReader dr(dim, path + ".tensor.shape[]", false, GE_STATUS_PLUGIN_MANIFEST_INVALID);
          std::int64_t min = 0, max = -1;
          if (Status s = dr.RequireInteger("min", &min); !s.ok()) return s;
          std::optional<std::int64_t> mx;
          if (Status s = dr.OptionalInteger("max", &mx); !s.ok()) return s;
          if (mx) max = *mx;
          if (Status s = dr.Finish(); !s.ok()) return s;
          tc.shape.push_back({min, max});
        } else {
          return tr.Error(path + ".tensor.shape elements must be integer or {min,max}");
        }
      }
    }
    if (Status s = tr.Finish(); !s.ok()) return s;
    out->tensor = std::move(tc);
  }
  if (const JsonValue* v = r.Get("memory"); v != nullptr) {
    ObjectReader mr(*v, path + ".memory", allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
    if (!mr.ok()) return r.WrongType("memory", "object");
    std::optional<std::vector<std::string>> kinds;
    if (Status s = mr.OptionalStringArray("kinds", &kinds); !s.ok()) return s;
    if (kinds) {
      std::vector<MemoryKind> parsed;
      for (const std::string& k : *kinds) {
        auto mk = ParseMemoryKind(k);
        if (!mk) return mr.Error("invalid memory kind '" + k + "' in " + path);
        parsed.push_back(*mk);
      }
      out->memory.kinds = std::move(parsed);
    }
    if (const JsonValue* ids = mr.Get("device_ids"); ids != nullptr) {
      if (!ids->is_array()) return mr.WrongType("device_ids", "array");
      std::vector<std::int32_t> parsed;
      for (const JsonValue& id : ids->as_array()) {
        if (!id.is_integer()) return mr.Error(path + ".memory.device_ids must be integers");
        parsed.push_back(static_cast<std::int32_t>(id.as_integer()));
      }
      out->memory.device_ids = std::move(parsed);
    }
    if (Status s = mr.OptionalBool("zero_copy_required", &out->memory.zero_copy_required); !s.ok()) return s;
    if (Status s = mr.Finish(); !s.ok()) return s;
  }
  std::optional<std::vector<std::string>> sync;
  if (Status s = r.OptionalStringArray("sync", &sync); !s.ok()) return s;
  if (sync) {
    std::vector<SyncPolicy> parsed;
    for (const std::string& p : *sync) {
      auto sp = ParseSyncPolicy(p);
      if (!sp) return r.Error("invalid sync policy '" + p + "' in " + path);
      parsed.push_back(*sp);
    }
    out->sync = std::move(parsed);
  }
  return r.Finish();
}

JsonValue PortJson(const PortCapability& p) {
  JsonObject o;
  o.emplace("name", JsonValue(p.name));
  o.emplace("type_tag", JsonValue(p.type_tag));
  if (p.direction == PortDirection::kInput && !p.required) o.emplace("required", JsonValue(false));
  if (p.cardinality == PortCardinality::kMulti) o.emplace("cardinality", JsonValue("multi"));
  if (p.dynamic_consumers) o.emplace("dynamic_consumers", JsonValue(true));
  if (p.video) {
    JsonObject v;
    if (p.video->pixel_formats) v.emplace("pixel_formats", StringsJson(*p.video->pixel_formats));
    if (p.video->width) v.emplace("width", RangeJson(*p.video->width));
    if (p.video->height) v.emplace("height", RangeJson(*p.video->height));
    if (p.video->fps) v.emplace("fps", RangeJson(*p.video->fps));
    if (p.video->color_spaces) v.emplace("color_spaces", StringsJson(*p.video->color_spaces));
    if (p.video->color_ranges) v.emplace("color_ranges", StringsJson(*p.video->color_ranges));
    if (p.video->hdr_modes) v.emplace("hdr_modes", StringsJson(*p.video->hdr_modes));
    o.emplace("video", JsonValue(std::move(v)));
  }
  if (p.audio) {
    JsonObject a;
    if (p.audio->sample_formats) a.emplace("sample_formats", StringsJson(*p.audio->sample_formats));
    if (p.audio->sample_rate) a.emplace("sample_rate", RangeJson(*p.audio->sample_rate));
    if (p.audio->channel_layouts) a.emplace("channel_layouts", StringsJson(*p.audio->channel_layouts));
    o.emplace("audio", JsonValue(std::move(a)));
  }
  if (p.tensor) {
    JsonObject t;
    if (p.tensor->dtypes) t.emplace("dtypes", StringsJson(*p.tensor->dtypes));
    if (p.tensor->layouts) t.emplace("layouts", StringsJson(*p.tensor->layouts));
    if (!p.tensor->shape.empty()) {
      JsonArray shape;
      for (const IntRange& d : p.tensor->shape) {
        if (d.min == d.max) {
          shape.push_back(JsonValue(d.min));
        } else {
          JsonObject dd;
          dd.emplace("min", JsonValue(d.min));
          if (d.max >= 0) dd.emplace("max", JsonValue(d.max));
          shape.push_back(JsonValue(std::move(dd)));
        }
      }
      t.emplace("shape", JsonValue(std::move(shape)));
    }
    o.emplace("tensor", JsonValue(std::move(t)));
  }
  if (p.memory.kinds || p.memory.device_ids || p.memory.zero_copy_required) {
    JsonObject m;
    if (p.memory.kinds) {
      JsonArray kinds;
      for (MemoryKind k : *p.memory.kinds) kinds.push_back(JsonValue(std::string(ToString(k))));
      m.emplace("kinds", JsonValue(std::move(kinds)));
    }
    if (p.memory.device_ids) {
      JsonArray ids;
      for (std::int32_t id : *p.memory.device_ids) ids.push_back(JsonValue(static_cast<std::int64_t>(id)));
      m.emplace("device_ids", JsonValue(std::move(ids)));
    }
    if (p.memory.zero_copy_required) m.emplace("zero_copy_required", JsonValue(true));
    o.emplace("memory", JsonValue(std::move(m)));
  }
  if (p.sync) {
    JsonArray s;
    for (SyncPolicy sp : *p.sync) s.push_back(JsonValue(std::string(ToString(sp))));
    o.emplace("sync", JsonValue(std::move(s)));
  }
  return JsonValue(std::move(o));
}

}  // namespace

Result<CapabilityDescriptor> CapabilityDescriptor::ParseJson(std::string_view json) {
  JsonParseResult parsed = ge::ParseJson(json);
  if (!parsed.ok()) return Status::PluginManifestInvalid("invalid capability JSON: " + parsed.error);
  return ParseJson(*parsed.value);
}

Result<CapabilityDescriptor> CapabilityDescriptor::ParseJson(const JsonValue& doc) {
  bool allow_unknown = false;
  if (Status s = internal::CheckDocumentHeader(doc, "CapabilityDescriptor", kSchemaVersion,
                                               kSchemaId, &allow_unknown,
                                               GE_STATUS_PLUGIN_MANIFEST_INVALID);
      !s.ok()) return s;
  ObjectReader r(doc, "CapabilityDescriptor", allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
  internal::MarkHeaderSeen(r);

  CapabilityDescriptor d;
  std::string op;
  if (Status s = r.RequireString("operator", &op); !s.ok()) return s;
  auto key = OperatorKey::Parse(op);
  if (!key) return r.Error("invalid operator '" + op + "'");
  d.op = std::move(*key);
  if (Status s = r.OptionalString("description", &d.description); !s.ok()) return s;

  const JsonValue* ports = r.Get("ports");
  if (ports == nullptr) return r.Missing("ports");
  ObjectReader pr(*ports, "ports", allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
  if (!pr.ok()) return r.WrongType("ports", "object");
  for (const auto& [key_name, dir, target] :
       {std::tuple{"inputs", PortDirection::kInput, &d.inputs},
        std::tuple{"outputs", PortDirection::kOutput, &d.outputs}}) {
    const JsonValue* arr = pr.Get(key_name);
    if (arr == nullptr) continue;
    if (!arr->is_array()) return pr.WrongType(key_name, "array");
    std::size_t i = 0;
    for (const JsonValue& p : arr->as_array()) {
      PortCapability port;
      if (Status s = ParsePort(p, std::string("ports.") + key_name + "[" + std::to_string(i++) + "]",
                               dir, allow_unknown, &port);
          !s.ok()) return s;
      for (const PortCapability& existing : *target) {
        if (existing.name == port.name) {
          return pr.Error("duplicate port name '" + port.name + "'");
        }
      }
      target->push_back(std::move(port));
    }
  }
  if (Status s = pr.Finish(); !s.ok()) return s;

  if (const JsonValue* ex = r.Get("execution"); ex != nullptr) {
    ObjectReader er(*ex, "execution", allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
    if (!er.ok()) return r.WrongType("execution", "object");
    std::vector<std::string> devices;
    if (Status s = er.StringArray("devices", false, &devices); !s.ok()) return s;
    for (const std::string& dev : devices) {
      auto dk = ParseDeviceKind(dev);
      if (!dk) return er.Error("invalid device '" + dev + "'");
      d.execution.devices.push_back(*dk);
    }
    if (Status s = er.OptionalBool("stateful", &d.execution.stateful); !s.ok()) return s;
    if (Status s = er.OptionalBool("async", &d.execution.async); !s.ok()) return s;
    std::optional<std::int64_t> mp;
    if (Status s = er.OptionalInteger("max_parallelism", &mp); !s.ok()) return s;
    if (mp) {
      if (*mp < 1) return er.Error("max_parallelism must be >= 1");
      d.execution.max_parallelism = static_cast<std::uint32_t>(*mp);
    }
    if (Status s = er.OptionalBool("zero_copy", &d.execution.zero_copy); !s.ok()) return s;
    std::optional<std::int64_t> mi;
    if (Status s = er.OptionalInteger("max_inference_ms", &mi); !s.ok()) return s;
    if (mi) d.execution.max_inference_ms = static_cast<std::uint64_t>(*mi);
    if (Status s = er.Finish(); !s.ok()) return s;
  }
  if (d.execution.stateful && d.execution.max_parallelism > 1) {
    return r.Error("stateful operator must have max_parallelism == 1");
  }
  if (d.execution.async && !d.execution.max_inference_ms) {
    return r.Error("async operator must declare max_inference_ms");
  }

  if (const JsonValue* pm = r.Get("parameters"); pm != nullptr) {
    ObjectReader mr(*pm, "parameters", allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
    if (!mr.ok()) return r.WrongType("parameters", "object");
    if (Status s = mr.StringArray("hot_updatable", false, &d.parameters.hot_updatable); !s.ok()) return s;
    if (Status s = mr.StringArray("migratable", false, &d.parameters.migratable); !s.ok()) return s;
    std::string schema_ref;
    if (Status s = mr.OptionalString("schema_ref", &schema_ref); !s.ok()) return s;
    if (Status s = mr.OptionalObject("schema", &d.parameters.schema); !s.ok()) return s;
    if (Status s = mr.Finish(); !s.ok()) return s;
  }

  if (const JsonValue* res = r.Get("resources"); res != nullptr) {
    if (!res->is_object()) return r.WrongType("resources", "object");
    for (const auto& [k, v] : res->as_object()) {
      if (!v.is_integer()) return r.Error("resources." + k + " must be integer");
      d.resources.amounts[k] = v.as_integer();
    }
  }
  if (const JsonValue* ev = r.Get("events"); ev != nullptr) {
    ObjectReader er(*ev, "events", allow_unknown, GE_STATUS_PLUGIN_MANIFEST_INVALID);
    if (!er.ok()) return r.WrongType("events", "object");
    if (Status s = er.StringArray("emits", false, &d.events.emits); !s.ok()) return s;
    if (Status s = er.StringArray("accepts", false, &d.events.accepts); !s.ok()) return s;
    if (Status s = er.Finish(); !s.ok()) return s;
  }
  if (Status s = r.Finish(); !s.ok()) return s;
  return d;
}

JsonValue CapabilityDescriptor::ToJson() const {
  JsonObject o;
  o.emplace("kind", JsonValue("CapabilityDescriptor"));
  o.emplace("schema_version", JsonValue(static_cast<std::int64_t>(kSchemaVersion)));
  o.emplace("$id", JsonValue(kSchemaId));
  o.emplace("operator", JsonValue(op.ToString()));
  if (!description.empty()) o.emplace("description", JsonValue(description));
  JsonObject ports;
  JsonArray ins, outs;
  for (const PortCapability& p : inputs) ins.push_back(PortJson(p));
  for (const PortCapability& p : outputs) outs.push_back(PortJson(p));
  ports.emplace("inputs", JsonValue(std::move(ins)));
  ports.emplace("outputs", JsonValue(std::move(outs)));
  o.emplace("ports", JsonValue(std::move(ports)));
  JsonObject ex;
  JsonArray devices;
  for (DeviceKind d : execution.devices) devices.push_back(JsonValue(std::string(ToString(d))));
  ex.emplace("devices", JsonValue(std::move(devices)));
  ex.emplace("stateful", JsonValue(execution.stateful));
  ex.emplace("async", JsonValue(execution.async));
  ex.emplace("max_parallelism", JsonValue(static_cast<std::int64_t>(execution.max_parallelism)));
  ex.emplace("zero_copy", JsonValue(execution.zero_copy));
  if (execution.max_inference_ms) {
    ex.emplace("max_inference_ms", JsonValue(static_cast<std::int64_t>(*execution.max_inference_ms)));
  }
  o.emplace("execution", JsonValue(std::move(ex)));
  JsonObject pm;
  pm.emplace("hot_updatable", StringsJson(parameters.hot_updatable));
  if (!parameters.migratable.empty()) pm.emplace("migratable", StringsJson(parameters.migratable));
  if (!parameters.schema.as_object().empty()) pm.emplace("schema", parameters.schema);
  o.emplace("parameters", JsonValue(std::move(pm)));
  if (!resources.amounts.empty()) {
    JsonObject res;
    for (const auto& [k, v] : resources.amounts) res.emplace(k, JsonValue(v));
    o.emplace("resources", JsonValue(std::move(res)));
  }
  if (!events.emits.empty() || !events.accepts.empty()) {
    JsonObject ev;
    ev.emplace("emits", StringsJson(events.emits));
    ev.emplace("accepts", StringsJson(events.accepts));
    o.emplace("events", JsonValue(std::move(ev)));
  }
  return JsonValue(std::move(o));
}

// ---------------------------------------------------------------------------
// ConnectionContract / Conflict / PreferenceSet
// ---------------------------------------------------------------------------

JsonValue ConnectionContract::ToJson() const {
  JsonObject o;
  o.emplace("logical_type", JsonValue(logical_type));
  if (video) {
    JsonObject v;
    v.emplace("pixel_format", JsonValue(video->pixel_format));
    if (video->width) v.emplace("width", RangeJson(*video->width));
    if (video->height) v.emplace("height", RangeJson(*video->height));
    if (video->fps) v.emplace("fps", RangeJson(*video->fps));
    if (video->color_space) v.emplace("color_space", JsonValue(*video->color_space));
    if (video->color_range) v.emplace("color_range", JsonValue(*video->color_range));
    if (video->hdr_mode) v.emplace("hdr_mode", JsonValue(*video->hdr_mode));
    o.emplace("video", JsonValue(std::move(v)));
  }
  if (audio) {
    JsonObject a;
    a.emplace("sample_format", JsonValue(audio->sample_format));
    if (audio->sample_rate) a.emplace("sample_rate", RangeJson(*audio->sample_rate));
    if (audio->channel_layout) a.emplace("channel_layout", JsonValue(*audio->channel_layout));
    o.emplace("audio", JsonValue(std::move(a)));
  }
  if (tensor) {
    JsonObject t;
    t.emplace("dtype", JsonValue(tensor->dtype));
    if (tensor->layout) t.emplace("layout", JsonValue(*tensor->layout));
    JsonArray shape;
    for (const IntRange& d : tensor->shape) shape.push_back(RangeJson(d));
    t.emplace("shape", JsonValue(std::move(shape)));
    o.emplace("tensor", JsonValue(std::move(t)));
  }
  o.emplace("memory_kind", JsonValue(std::string(ToString(memory_kind))));
  o.emplace("device_id", JsonValue(static_cast<std::int64_t>(device_id)));
  o.emplace("sync_policy", JsonValue(std::string(ToString(sync_policy))));
  o.emplace("source_capability", JsonValue(source_capability));
  o.emplace("target_capability", JsonValue(target_capability));
  return JsonValue(std::move(o));
}

JsonValue CapabilityConflict::ToJson() const {
  JsonObject o;
  o.emplace("dimension", JsonValue(dimension));
  o.emplace("output", JsonValue(lhs_json));
  o.emplace("input", JsonValue(rhs_json));
  if (!suggested_converter.empty()) o.emplace("suggested_converter", JsonValue(suggested_converter));
  if (!consumers.empty()) o.emplace("consumers", StringsJson(consumers));
  return JsonValue(std::move(o));
}

std::uint64_t PreferenceSet::Hash() const {
  JsonObject o;
  for (const auto& [k, v] : ordered) o.emplace(k, StringsJson(v));
  if (memory_kind) o.emplace("memory_kind", JsonValue(std::string(ToString(*memory_kind))));
  if (device_id) o.emplace("device_id", JsonValue(static_cast<std::int64_t>(*device_id)));
  if (sync_policy) o.emplace("sync_policy", JsonValue(std::string(ToString(*sync_policy))));
  return internal::Fnv1a(JsonValue(std::move(o)).Serialize());
}

}  // namespace ge
