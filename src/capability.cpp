#include "ge/cpp/capability.h"

#include <algorithm>
#include <functional>

#include "capability_internal.h"
#include "json_reader.h"

namespace ge {

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
  return s ? internal::StringsJson(*s).Serialize() : "\"*\"";
}
template <typename R>
std::string RangeStr(const std::optional<R>& r) {
  return r ? internal::RangeJson(*r).Serialize() : "\"*\"";
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
              return conflict("shape[" + std::to_string(i) + "]", internal::RangeJson(common.shape[i]).Serialize(), internal::RangeJson(o).Serialize());
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
        return internal::StringsJson(names).Serialize();
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
        return conflict("sync_policy", "\"" + std::string(ToString(*want)) + "\"", internal::StringsJson(names).Serialize());
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
