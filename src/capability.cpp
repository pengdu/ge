#include "ge/cpp/capability.h"

#include <algorithm>
#include <functional>

#include "json_reader.h"

namespace ge {

using internal::ObjectReader;

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

std::string_view ToString(MemoryKind kind) noexcept {
  switch (kind) {
    case MemoryKind::kHost: return "host";
    case MemoryKind::kPinned: return "pinned";
    case MemoryKind::kCudaDevice: return "cuda_device";
    case MemoryKind::kDmaBuf: return "dmabuf";
  }
  return "host";
}
std::optional<MemoryKind> ParseMemoryKind(std::string_view s) {
  if (s == "host") return MemoryKind::kHost;
  if (s == "pinned") return MemoryKind::kPinned;
  if (s == "cuda_device") return MemoryKind::kCudaDevice;
  if (s == "dmabuf") return MemoryKind::kDmaBuf;
  return std::nullopt;
}
std::string_view ToString(DeviceKind kind) noexcept {
  return kind == DeviceKind::kGpu ? "gpu" : "cpu";
}
std::optional<DeviceKind> ParseDeviceKind(std::string_view s) {
  if (s == "cpu") return DeviceKind::kCpu;
  if (s == "gpu") return DeviceKind::kGpu;
  return std::nullopt;
}

bool IsBuiltinTypeTag(std::string_view tag) noexcept {
  return tag == "VideoFrame" || tag == "AudioFrame" || tag == "Tensor" ||
         tag == "Bytes" || tag == "Json" || tag == "EncodedVideo" ||
         tag == "EncodedAudio";
}

// ---------------------------------------------------------------------------
// Descriptor: lookup / version
// ---------------------------------------------------------------------------

const PortCapability* CapabilityDescriptor::FindInput(std::string_view name) const noexcept {
  for (const PortCapability& p : inputs) {
    if (p.name == name) return &p;
  }
  return nullptr;
}
const PortCapability* CapabilityDescriptor::FindOutput(std::string_view name) const noexcept {
  for (const PortCapability& p : outputs) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

CapabilityVersion CapabilityDescriptor::Version() const {
  return internal::Fnv1a(Serialize());
}

// ---------------------------------------------------------------------------
// Descriptor: JSON
// ---------------------------------------------------------------------------

namespace {

JsonValue RangeJson(const IntRange& r) {
  JsonObject o;
  o.emplace("min", JsonValue(r.min));
  o.emplace("max", JsonValue(r.max));
  return JsonValue(std::move(o));
}
JsonValue RangeJson(const RationalRange& r) {
  JsonObject o;
  o.emplace("min", JsonValue(r.min));
  o.emplace("max", JsonValue(r.max));
  return JsonValue(std::move(o));
}
JsonValue StringsJson(const std::vector<std::string>& v) {
  JsonArray a;
  for (const std::string& s : v) a.push_back(JsonValue(s));
  return JsonValue(std::move(a));
}

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

// ---------------------------------------------------------------------------
// Negotiation core
// ---------------------------------------------------------------------------

namespace {

// Intersection preserving |lhs| order. nullopt on either side = unconstrained.
std::optional<FormatSet> IntersectSets(const std::optional<FormatSet>& lhs,
                                       const std::optional<FormatSet>& rhs) {
  if (!lhs) return rhs;
  if (!rhs) return lhs;
  FormatSet out;
  for (const std::string& l : *lhs) {
    if (std::find(rhs->begin(), rhs->end(), l) != rhs->end()) out.push_back(l);
  }
  return out;
}

template <typename R>
std::optional<R> IntersectRanges(const std::optional<R>& lhs, const std::optional<R>& rhs) {
  if (!lhs) return rhs;
  if (!rhs) return lhs;
  return lhs->Intersect(*rhs);
}

std::string SetJson(const std::optional<FormatSet>& s) {
  return s ? StringsJson(*s).Serialize() : "\"*\"";
}
template <typename R>
std::string RangeStr(const std::optional<R>& r) {
  return r ? RangeJson(*r).Serialize() : "\"*\"";
}

// 12 §8.2 step 5: caller preference -> input priority -> output priority
// -> stable lexical order. |common| is already in *output* priority order
// (left operand of the intersection), so we only need to re-rank by caller
// preference and input priority.
std::string ChooseDeterministically(const FormatSet& common,
                                    const FormatSet* caller_pref,
                                    const std::optional<FormatSet>& input_priority) {
  if (caller_pref != nullptr) {
    for (const std::string& p : *caller_pref) {
      if (std::find(common.begin(), common.end(), p) != common.end()) return p;
    }
  }
  if (input_priority) {
    for (const std::string& p : *input_priority) {
      if (std::find(common.begin(), common.end(), p) != common.end()) return p;
    }
  }
  return common.front();
}

const FormatSet* Pref(const PreferenceSet& pref, const char* dim) {
  const auto it = pref.ordered.find(dim);
  return it == pref.ordered.end() ? nullptr : &it->second;
}

std::string SuggestConverter(std::string_view dimension, std::string_view type_tag) {
  if (dimension == "logical_type") return "";
  if (type_tag == "VideoFrame") {
    if (dimension == "width" || dimension == "height") return "VideoScale";
    if (dimension == "fps") return "VideoRate";
    if (dimension == "memory_kind" || dimension == "device_id") return "MemoryCopy";
    return "VideoConvert";
  }
  if (type_tag == "AudioFrame") {
    if (dimension == "memory_kind" || dimension == "device_id") return "MemoryCopy";
    return "AudioResample";
  }
  if (type_tag == "Tensor") {
    if (dimension == "memory_kind" || dimension == "device_id") return "MemoryCopy";
    return "TensorConvert";
  }
  if (dimension == "memory_kind" || dimension == "device_id") return "MemoryCopy";
  return "";
}

}  // namespace

Result<ConnectionContract> CapabilityNegotiator::NegotiateUncached(
    const PortCapability& output, const std::vector<const PortCapability*>& inputs,
    std::optional<SyncPolicy> edge_sync, const PreferenceSet& pref,
    const std::vector<std::string>& consumer_names) {
  last_conflict_.reset();
  const auto conflict = [&](std::string dim, std::string lhs, std::string rhs) -> Status {
    CapabilityConflict c;
    c.dimension = dim;
    c.lhs_json = std::move(lhs);
    c.rhs_json = std::move(rhs);
    c.suggested_converter = SuggestConverter(dim, output.type_tag);
    c.consumers = consumer_names;
    Status s = Status::CapabilityConflict(
        "no common capability on dimension '" + dim + "'" +
            (c.suggested_converter.empty() ? "" : " (insert explicit " + c.suggested_converter + ")"),
        c.ToJson().Serialize());
    last_conflict_ = std::move(c);
    return s;
  };

  // 1. direction + logical type
  if (output.direction != PortDirection::kOutput) {
    return Status::InvalidArgument("negotiation source must be an output port");
  }
  for (const PortCapability* in : inputs) {
    if (in->direction != PortDirection::kInput) {
      return Status::InvalidArgument("negotiation target must be an input port");
    }
    if (in->type_tag != output.type_tag) {
      return conflict("logical_type", "\"" + output.type_tag + "\"", "\"" + in->type_tag + "\"");
    }
  }

  ConnectionContract contract;
  contract.logical_type = output.type_tag;

  // 2/3. applicable dimensions. Opaque tags: only logical_type (CAP-7).
  const bool builtin = IsBuiltinTypeTag(output.type_tag);

  if (builtin && (output.video || std::any_of(inputs.begin(), inputs.end(),
                                              [](auto* i) { return i->video.has_value(); }))) {
    VideoConstraints common = output.video.value_or(VideoConstraints{});
    for (const PortCapability* in : inputs) {
      const VideoConstraints iv = in->video.value_or(VideoConstraints{});
      common.pixel_formats = IntersectSets(common.pixel_formats, iv.pixel_formats);
      common.width = IntersectRanges(common.width, iv.width);
      common.height = IntersectRanges(common.height, iv.height);
      common.fps = IntersectRanges(common.fps, iv.fps);
      common.color_spaces = IntersectSets(common.color_spaces, iv.color_spaces);
      common.color_ranges = IntersectSets(common.color_ranges, iv.color_ranges);
      common.hdr_modes = IntersectSets(common.hdr_modes, iv.hdr_modes);
    }
    const auto in_pf = inputs.empty() ? std::nullopt
                                      : inputs.front()->video.value_or(VideoConstraints{}).pixel_formats;
    if (!common.pixel_formats || common.pixel_formats->empty()) {
      if (!common.pixel_formats) {
        return conflict("pixel_format", "\"*\"", "\"*\"");  // required dim missing on both sides
      }
      return conflict("pixel_format", SetJson(output.video ? output.video->pixel_formats : std::nullopt),
                      SetJson(in_pf));
    }
    if (common.width && common.width->Empty()) {
      return conflict("width", RangeStr(output.video ? output.video->width : std::nullopt),
                      RangeStr(inputs.front()->video ? inputs.front()->video->width : std::nullopt));
    }
    if (common.height && common.height->Empty()) {
      return conflict("height", RangeStr(output.video ? output.video->height : std::nullopt),
                      RangeStr(inputs.front()->video ? inputs.front()->video->height : std::nullopt));
    }
    if (common.fps && common.fps->Empty()) {
      return conflict("fps", RangeStr(output.video ? output.video->fps : std::nullopt),
                      RangeStr(inputs.front()->video ? inputs.front()->video->fps : std::nullopt));
    }
    for (const auto& [name, set] : {std::pair{"color_space", &common.color_spaces},
                                    std::pair{"color_range", &common.color_ranges},
                                    std::pair{"hdr_mode", &common.hdr_modes}}) {
      if (*set && (*set)->empty()) return conflict(name, "[]", "[]");
    }
    SelectedVideoFormat sel;
    sel.pixel_format = ChooseDeterministically(*common.pixel_formats, Pref(pref, "pixel_format"), in_pf);
    sel.width = common.width;
    sel.height = common.height;
    sel.fps = common.fps;
    if (common.color_spaces) sel.color_space = ChooseDeterministically(*common.color_spaces, Pref(pref, "color_space"), std::nullopt);
    if (common.color_ranges) sel.color_range = ChooseDeterministically(*common.color_ranges, Pref(pref, "color_range"), std::nullopt);
    if (common.hdr_modes) sel.hdr_mode = ChooseDeterministically(*common.hdr_modes, Pref(pref, "hdr_mode"), std::nullopt);
    contract.video = std::move(sel);
  }

  if (builtin && (output.audio || std::any_of(inputs.begin(), inputs.end(),
                                              [](auto* i) { return i->audio.has_value(); }))) {
    AudioConstraints common = output.audio.value_or(AudioConstraints{});
    for (const PortCapability* in : inputs) {
      const AudioConstraints ia = in->audio.value_or(AudioConstraints{});
      common.sample_formats = IntersectSets(common.sample_formats, ia.sample_formats);
      common.sample_rate = IntersectRanges(common.sample_rate, ia.sample_rate);
      common.channel_layouts = IntersectSets(common.channel_layouts, ia.channel_layouts);
    }
    const auto in_sf = inputs.empty() ? std::nullopt
                                      : inputs.front()->audio.value_or(AudioConstraints{}).sample_formats;
    if (!common.sample_formats || common.sample_formats->empty()) {
      return conflict("sample_format", SetJson(output.audio ? output.audio->sample_formats : std::nullopt), SetJson(in_sf));
    }
    if (common.sample_rate && common.sample_rate->Empty()) {
      return conflict("sample_rate", RangeStr(output.audio ? output.audio->sample_rate : std::nullopt),
                      RangeStr(inputs.front()->audio ? inputs.front()->audio->sample_rate : std::nullopt));
    }
    if (common.channel_layouts && common.channel_layouts->empty()) return conflict("channel_layout", "[]", "[]");
    SelectedAudioFormat sel;
    sel.sample_format = ChooseDeterministically(*common.sample_formats, Pref(pref, "sample_format"), in_sf);
    sel.sample_rate = common.sample_rate;
    if (common.channel_layouts) sel.channel_layout = ChooseDeterministically(*common.channel_layouts, Pref(pref, "channel_layout"), std::nullopt);
    contract.audio = std::move(sel);
  }

  if (builtin && (output.tensor || std::any_of(inputs.begin(), inputs.end(),
                                               [](auto* i) { return i->tensor.has_value(); }))) {
    TensorConstraints common = output.tensor.value_or(TensorConstraints{});
    for (const PortCapability* in : inputs) {
      const TensorConstraints it = in->tensor.value_or(TensorConstraints{});
      common.dtypes = IntersectSets(common.dtypes, it.dtypes);
      common.layouts = IntersectSets(common.layouts, it.layouts);
      if (!it.shape.empty()) {
        if (common.shape.empty()) {
          common.shape = it.shape;
        } else if (common.shape.size() != it.shape.size()) {
          return conflict("shape_rank", std::to_string(common.shape.size()), std::to_string(it.shape.size()));
        } else {
          for (std::size_t i = 0; i < common.shape.size(); ++i) {
            IntRange& c = common.shape[i];
            const IntRange& o = it.shape[i];
            c.min = std::max(c.min, o.min);
            if (c.max < 0) c.max = o.max;
            else if (o.max >= 0) c.max = std::min(c.max, o.max);
            if (c.max >= 0 && c.max < c.min) {
              return conflict("shape[" + std::to_string(i) + "]", RangeJson(common.shape[i]).Serialize(), RangeJson(o).Serialize());
            }
          }
        }
      }
    }
    const auto in_dt = inputs.empty() ? std::nullopt
                                      : inputs.front()->tensor.value_or(TensorConstraints{}).dtypes;
    if (!common.dtypes || common.dtypes->empty()) {
      return conflict("dtype", SetJson(output.tensor ? output.tensor->dtypes : std::nullopt), SetJson(in_dt));
    }
    if (common.layouts && common.layouts->empty()) return conflict("layout", "[]", "[]");
    SelectedTensorFormat sel;
    sel.dtype = ChooseDeterministically(*common.dtypes, Pref(pref, "dtype"), in_dt);
    if (common.layouts) sel.layout = ChooseDeterministically(*common.layouts, Pref(pref, "layout"), std::nullopt);
    sel.shape = common.shape;
    contract.tensor = std::move(sel);
  }

  // memory kind / device
  {
    std::optional<std::vector<MemoryKind>> kinds = output.memory.kinds;
    std::optional<std::vector<std::int32_t>> devices = output.memory.device_ids;
    for (const PortCapability* in : inputs) {
      if (in->memory.kinds) {
        if (!kinds) {
          kinds = in->memory.kinds;
        } else {
          std::vector<MemoryKind> common;
          for (MemoryKind k : *kinds) {
            if (std::find(in->memory.kinds->begin(), in->memory.kinds->end(), k) != in->memory.kinds->end()) common.push_back(k);
          }
          kinds = std::move(common);
        }
      }
      if (in->memory.device_ids) {
        if (!devices) {
          devices = in->memory.device_ids;
        } else {
          std::vector<std::int32_t> common;
          for (std::int32_t d : *devices) {
            if (std::find(in->memory.device_ids->begin(), in->memory.device_ids->end(), d) != in->memory.device_ids->end()) common.push_back(d);
          }
          devices = std::move(common);
        }
      }
    }
    if (kinds && kinds->empty()) {
      const auto ks = [](const std::optional<std::vector<MemoryKind>>& v) {
        if (!v) return std::string("\"*\"");
        std::vector<std::string> names;
        for (MemoryKind k : *v) names.emplace_back(ToString(k));
        return StringsJson(names).Serialize();
      };
      return conflict("memory_kind", ks(output.memory.kinds), ks(inputs.front()->memory.kinds));
    }
    if (!kinds) {
      contract.memory_kind = pref.memory_kind.value_or(MemoryKind::kHost);
    } else if (pref.memory_kind && std::find(kinds->begin(), kinds->end(), *pref.memory_kind) != kinds->end()) {
      contract.memory_kind = *pref.memory_kind;
    } else {
      contract.memory_kind = kinds->front();
    }
    if (devices && devices->empty()) return conflict("device_id", "[]", "[]");
    if (contract.memory_kind == MemoryKind::kCudaDevice || contract.memory_kind == MemoryKind::kPinned) {
      if (!devices) {
        contract.device_id = pref.device_id.value_or(0);
      } else if (pref.device_id && std::find(devices->begin(), devices->end(), *pref.device_id) != devices->end()) {
        contract.device_id = *pref.device_id;
      } else {
        contract.device_id = *std::min_element(devices->begin(), devices->end());
      }
    } else {
      contract.device_id = -1;
    }
  }

  // sync policy: edge override must be supported by every input.
  {
    std::optional<std::vector<SyncPolicy>> supported;
    for (const PortCapability* in : inputs) {
      if (!in->sync) continue;
      if (!supported) {
        supported = in->sync;
      } else {
        std::vector<SyncPolicy> common;
        for (SyncPolicy p : *supported) {
          if (std::find(in->sync->begin(), in->sync->end(), p) != in->sync->end()) common.push_back(p);
        }
        supported = std::move(common);
      }
    }
    const std::optional<SyncPolicy> want = edge_sync ? edge_sync : pref.sync_policy;
    if (supported && supported->empty()) return conflict("sync_policy", "[]", "[]");
    if (want) {
      if (supported && std::find(supported->begin(), supported->end(), *want) == supported->end()) {
        std::vector<std::string> names;
        for (SyncPolicy p : *supported) names.emplace_back(ToString(p));
        return conflict("sync_policy", "\"" + std::string(ToString(*want)) + "\"", StringsJson(names).Serialize());
      }
      contract.sync_policy = *want;
    } else {
      contract.sync_policy = supported ? supported->front() : SyncPolicy::kAny;
    }
  }

  return contract;
}

Result<ConnectionContract> CapabilityNegotiator::NegotiateEdge(
    const OperatorKey& source_op, CapabilityVersion source_cap,
    const PortCapability& output, const OperatorKey& target_op,
    CapabilityVersion target_cap, const PortCapability& input,
    std::optional<SyncPolicy> edge_sync, const PreferenceSet& pref) {
  NegotiationCacheKey key{source_op, source_cap, target_op, target_cap,
                          output.name, input.name, pref.Hash()};
  if (edge_sync) key.preferences ^= static_cast<std::uint64_t>(*edge_sync) + 0x9e3779b97f4a7c15ULL;
  if (const auto it = cache_.find(key); it != cache_.end()) return it->second;

  auto r = NegotiateUncached(output, {&input}, edge_sync, pref, {});
  if (!r.ok()) return r.status();
  r->source_capability = source_cap;
  r->target_capability = target_cap;
  cache_.emplace(std::move(key), *r);
  return r;
}

Result<ConnectionContract> CapabilityNegotiator::NegotiateFanout(
    const OperatorKey& /*source_op*/, CapabilityVersion source_cap,
    const PortCapability& output, std::vector<FanoutConsumer> consumers,
    const PreferenceSet& pref, const ConnectionContract* existing) {
  if (consumers.empty()) return Status::InvalidArgument("fan-out needs at least one consumer");
  std::sort(consumers.begin(), consumers.end(), [](const FanoutConsumer& a, const FanoutConsumer& b) {
    return std::tie(a.node_id, a.port->name) < std::tie(b.node_id, b.port->name);
  });
  std::vector<const PortCapability*> inputs;
  std::vector<std::string> names;
  std::optional<SyncPolicy> edge_sync;
  CapabilityVersion targets_hash = 0;
  for (const FanoutConsumer& c : consumers) {
    inputs.push_back(c.port);
    names.push_back(c.node_id + "." + c.port->name);
    if (c.edge_sync) {
      if (edge_sync && *edge_sync != *c.edge_sync) {
        return Status::CapabilityConflict("fan-out consumers request different sync policies");
      }
      edge_sync = c.edge_sync;
    }
    targets_hash = internal::Fnv1a(c.node_id + "." + c.port->name + ":" + std::to_string(c.capability), targets_hash ^ 0x9e3779b97f4a7c15ULL);
  }
  auto r = NegotiateUncached(output, inputs, edge_sync, pref, names);
  if (!r.ok()) return r.status();
  r->source_capability = source_cap;
  r->target_capability = targets_hash;
  if (existing != nullptr) {
    ConnectionContract candidate = *r;
    candidate.target_capability = existing->target_capability;
    if (candidate != *existing) {
      CapabilityConflict c;
      c.dimension = "existing_contract";
      c.lhs_json = existing->Serialize();
      c.rhs_json = r->Serialize();
      c.consumers = names;
      last_conflict_ = c;
      return Status::CapabilityConflict(
          "adding consumer would change the published fan-out contract", c.ToJson().Serialize());
    }
  }
  return r;
}

void CapabilityNegotiator::InvalidateOperator(const OperatorKey& op) {
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (it->first.source_operator == op || it->first.target_operator == op) {
      it = cache_.erase(it);
    } else {
      ++it;
    }
  }
}

void CapabilityNegotiator::InvalidateAll() { cache_.clear(); }

}  // namespace ge
