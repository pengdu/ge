#include <ge/media/mix.h>

#include <algorithm>
#include <map>
#include <optional>

#include <ge/media/operators.h>

namespace ge::media {

namespace {

PortRef Port(std::string_view text) {
  const std::optional<PortRef> p = PortRef::Parse(text);
  return p ? *p : PortRef{};
}

EdgeSpec Edge(std::string id, PortRef from, PortRef to, std::uint32_t capacity) {
  EdgeSpec e;
  e.id = std::move(id);
  e.from = std::move(from);
  e.to = std::move(to);
  e.queue.capacity = capacity;
  e.queue.policy = DropPolicy::kBlock;
  return e;
}

OperatorKey Key(std::string_view text) { return *OperatorKey::Parse(text); }

std::string FitName(MixFit fit) { return fit == MixFit::kContain ? "contain" : "cover"; }

JsonValue RectJson(const MixRect& r) {
  return JsonValue(JsonObject{{"x", JsonValue(r.x)}, {"y", JsonValue(r.y)}, {"w", JsonValue(r.w)}, {"h", JsonValue(r.h)}});
}

// The slot's explicit port when it has one, otherwise its member id.
std::string PortOrMember(const SlotSpec& s) { return s.port.empty() ? s.member_id : s.port; }

std::uint32_t Cap(const MixOptions& o) { return static_cast<std::uint32_t>(o.queue_capacity); }

std::string MemberNodeId(std::string_view member_id, std::string_view suffix) {
  return "m." + std::string(member_id) + "." + std::string(suffix);
}

}  // namespace

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

MixRect GridCell(int index, int count, int width, int height, std::int64_t gap) {
  if (count <= 1) return MixRect{0, 0, width, height};
  const int g = static_cast<int>(gap);
  if (count == 2) {
    const int left = (width - g) / 2;
    return index == 0 ? MixRect{0, 0, left, height} : MixRect{left + g, 0, width - left - g, height};
  }
  if (count == 3) {
    // 2 + 1: the speaker takes the top half, the other two split the bottom.
    const int half_h = (height - g) / 2;
    if (index == 0) return MixRect{0, 0, width, half_h};
    const int left = (width - g) / 2;
    return index == 1 ? MixRect{0, half_h + g, left, height - half_h - g}
                      : MixRect{left + g, half_h + g, width - left - g, height - half_h - g};
  }
  int cols = 1;
  while (cols * cols < count) ++cols;
  const int rows = (count + cols - 1) / cols;
  const int cw = (width - g * (cols - 1)) / cols;
  const int ch = (height - g * (rows - 1)) / rows;
  const int row = index / cols;
  const int col = index % cols;
  return MixRect{col * (cw + g), row * (ch + g), cw, ch};
}

std::vector<SlotSpec> ResolveLayout(const std::vector<SlotSpec>& slots, int width, int height, std::int64_t gap) {
  std::vector<SlotSpec> out = slots;
  std::vector<std::size_t> automatic;
  int band_top = 0;
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i].rect.w <= 0 || out[i].rect.h <= 0) {
      automatic.push_back(i);
      continue;
    }
    // Explicit windows keep their place across re-layouts (MX-L-4); the
    // automatic members grid into the band below them.
    band_top = std::max(band_top, out[i].rect.y + out[i].rect.h);
  }
  if (automatic.empty()) return out;
  const int band_height = std::max(0, height - band_top);
  for (std::size_t k = 0; k < automatic.size(); ++k) {
    if (band_height == 0) {
      out[automatic[k]].visible = false;
      continue;
    }
    MixRect cell = GridCell(static_cast<int>(k), static_cast<int>(automatic.size()), width, band_height, gap);
    cell.y += band_top;
    out[automatic[k]].rect = cell;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Template
// ---------------------------------------------------------------------------

std::string MixNodeIds::MemberSource(std::string_view member_id) const { return MemberNodeId(member_id, "src"); }
std::string MixNodeIds::MemberAudioSource(std::string_view member_id) const { return MemberNodeId(member_id, "asrc"); }
std::string MixNodeIds::MemberDecode(std::string_view member_id) const { return MemberNodeId(member_id, "dec"); }
std::string MixNodeIds::MemberAudioDecode(std::string_view member_id) const { return MemberNodeId(member_id, "adec"); }
std::string MixNodeIds::MemberEntryEdge(std::string_view member_id) const { return "e." + std::string(member_id) + ".v"; }
std::string MixNodeIds::MemberAudioEntryEdge(std::string_view member_id) const {
  return "e." + std::string(member_id) + ".a";
}
std::string MixNodeIds::MemberComposeEdge(std::string_view member_id, std::string_view port) const {
  return "e." + std::string(member_id) + "." + std::string(port);
}

std::string MixTemplate::MemberPort(int index) {
  return index <= 0 ? std::string(kMainPort) : "s" + std::to_string(index);
}

const char* MixTemplate::MemberSyncPort() { return kMainPort; }

Status MixTemplate::ValidateMember(const MixSpec& spec) {
  if (!IsValidStableId(spec.member_id) || spec.member_id.size() > 64) {
    return Status::InvalidArgument("member id '" + spec.member_id + "' is not a valid stable id");
  }
  if (spec.video_path.empty()) return Status::InvalidArgument("member '" + spec.member_id + "': video_path is required");
  if (spec.has_rect && (spec.rect.w <= 0 || spec.rect.h <= 0)) {
    return Status::InvalidArgument("member '" + spec.member_id + "': rect must have positive w/h");
  }
  if (spec.start_ms < 0 || spec.end_ms < 0 || (spec.end_ms > 0 && spec.end_ms <= spec.start_ms)) {
    return Status::InvalidArgument("member '" + spec.member_id + "': invalid start_ms/end_ms range");
  }
  if (spec.gain_millis < 0 || spec.gain_millis > 10000) {
    return Status::InvalidArgument("member '" + spec.member_id + "': gain_millis must be within 0..10000");
  }
  return Status::Ok();
}

std::vector<SlotSpec> MixTemplate::Slots(const std::vector<MixSpec>& members, const LayoutSpec& layout) {
  std::map<std::string, SlotSpec> overrides;
  for (const SlotSpec& s : layout.slots) overrides.emplace(s.member_id, s);
  std::vector<SlotSpec> out;
  out.reserve(members.size());
  for (const MixSpec& m : members) {
    SlotSpec s;
    s.member_id = m.member_id;
    if (m.has_rect) {
      s.rect = m.rect;
      s.fit = m.fit;
      s.z = m.z;
    }
    if (m.has_gain) {
      s.gain_millis = m.gain_millis;
      s.has_gain = true;
    }
    // Explicit host overrides win over the member's own request.
    if (const auto it = overrides.find(m.member_id); it != overrides.end()) {
      if (it->second.rect.w > 0 && it->second.rect.h > 0) s.rect = it->second.rect;
      s.fit = it->second.fit;
      s.z = it->second.z;
      s.visible = it->second.visible;
      if (it->second.has_gain) {
        s.gain_millis = it->second.gain_millis;
        s.has_gain = true;
      }
    }
    out.push_back(std::move(s));
  }
  return ResolveLayout(out, layout.width, layout.height, layout.gap);
}

NodeSpec MixTemplate::MemberDecodeNode(const MixSpec& spec) {
  NodeSpec n;
  n.id = MixNodeIds{}.MemberSource(spec.member_id);
  n.op = Key(kOpMediaDemux);
  JsonObject o{{"input_path", JsonValue(spec.video_path)}};
  if (spec.start_ms > 0) o.emplace("start_ms", JsonValue(spec.start_ms));
  if (spec.end_ms > 0) o.emplace("end_ms", JsonValue(spec.end_ms));
  n.options = JsonValue(std::move(o));
  return n;
}

NodeSpec MixTemplate::MemberAudioDecodeNode(const MixSpec& spec) {
  NodeSpec n;
  n.id = MixNodeIds{}.MemberAudioSource(spec.member_id);
  n.op = Key(kOpMediaDemux);
  const std::string path = spec.audio_path.empty() ? spec.video_path : spec.audio_path;
  JsonObject o{{"input_path", JsonValue(path)}};
  if (spec.start_ms > 0) o.emplace("start_ms", JsonValue(spec.start_ms));
  if (spec.end_ms > 0) o.emplace("end_ms", JsonValue(spec.end_ms));
  n.options = JsonValue(std::move(o));
  return n;
}

JsonValue MixTemplate::ComposeParameters(const LayoutSpec& layout, const MixOptions& options) {
  const std::vector<SlotSpec> slots = ResolveLayout(layout.slots, layout.width, layout.height, layout.gap);
  JsonArray arr;
  for (const SlotSpec& s : slots) {
    arr.push_back(JsonValue(JsonObject{{"member", JsonValue(PortOrMember(s))},
                                       {"rect", RectJson(s.rect)},
                                       {"fit", JsonValue(FitName(s.fit))},
                                       {"z", JsonValue(s.z)},
                                       {"visible", JsonValue(s.visible)}}));
  }
  return JsonValue(JsonObject{{"width", JsonValue(layout.width)},
                              {"height", JsonValue(layout.height)},
                              {"fps", JsonValue(layout.fps)},
                              {"queue_ms", JsonValue(options.queue_ms)},
                              {"gap", JsonValue(layout.gap)},
                              {"background", JsonValue(layout.background)},
                              {"slots", JsonValue(std::move(arr))},
                              {"window_ms", JsonValue(options.window_ms)},
                              {"reference", JsonValue(options.reference)},
                              {"on_missing", JsonValue(options.on_missing)},
                              {"freeze_upgrade_ms", JsonValue(options.freeze_upgrade_ms)},
                              {"drop_late", JsonValue(options.drop_late)}});
}

JsonValue MixTemplate::ComposeHotParameters(const LayoutSpec& layout, const MixOptions& options) {
  JsonValue full = ComposeParameters(layout, options);
  JsonObject hot;
  for (const char* key : {"slots", "gap", "background", "window_ms", "on_missing", "freeze_upgrade_ms",
                          "reference", "drop_late"}) {
    if (const JsonValue* v = full.Find(key)) hot.emplace(key, *v);
  }
  return JsonValue(std::move(hot));
}

JsonValue MixTemplate::MixParameters(const MixOptions& options, const std::vector<SlotSpec>& slots) {
  JsonArray arr;
  for (const SlotSpec& s : slots) {
    if (!s.has_gain) continue;
    arr.push_back(JsonValue(JsonObject{{"member", JsonValue(PortOrMember(s))}, {"gain_millis", JsonValue(s.gain_millis)}}));
  }
  (void)options;
  return JsonValue(JsonObject{{"gains", JsonValue(std::move(arr))}});
}

LayoutSpec MixTemplate::EffectiveLayout(const LayoutSpec& layout, const std::vector<MixSpec>& members) {
  LayoutSpec out = layout;
  out.slots = Slots(members, layout);
  for (std::size_t i = 0; i < out.slots.size() && i < members.size(); ++i) {
    out.slots[i].port = MemberPort(static_cast<int>(i));
  }
  return out;
}

NodeSpec MixTemplate::ComposeNode(const LayoutSpec& layout, const MixOptions& options) {
  NodeSpec n;
  n.id = MixNodeIds{}.compose;
  n.op = Key(kOpVideoCompose);
  n.options = ComposeParameters(layout, options);
  return n;
}

NodeSpec MixTemplate::MixNode(const LayoutSpec& layout, const MixOptions& options) {
  NodeSpec n;
  n.id = MixNodeIds{}.amix;
  n.op = Key(kOpAudioMix);
  n.options = MixParameters(options, ResolveLayout(layout.slots, layout.width, layout.height, layout.gap));
  return n;
}

NodeSpec MixTemplate::EncodeNode(const MixResultSpec& result) {
  NodeSpec n;
  n.id = MixNodeIds{}.venc;
  n.op = Key(kOpVideoEncode);
  n.options = JsonValue(JsonObject{{"codec", JsonValue(result.codec)},
                                   {"bitrate_kbps", JsonValue(result.bitrate_kbps)},
                                   {"gop", JsonValue(result.gop)},
                                   {"preset", JsonValue(result.preset)}});
  return n;
}

NodeSpec MixTemplate::AudioEncodeNode(const MixResultSpec& result) {
  NodeSpec n;
  n.id = "aenc";
  n.op = Key(kOpAudioEncode);
  n.options = JsonValue(JsonObject{{"bitrate_kbps", JsonValue(result.audio_bitrate_kbps)},
                                   {"sample_rate", JsonValue(result.audio_sample_rate)},
                                   {"channels", JsonValue(result.audio_channels)}});
  return n;
}

NodeSpec MixTemplate::MuxNode(const MixResultSpec& result) {
  NodeSpec n;
  n.id = MixNodeIds{}.mux;
  n.op = Key(kOpMediaMux);
  JsonObject o{{"output_path", JsonValue(result.output_path)}, {"container", JsonValue(result.container)}};
  if (result.container == "mp4") o.emplace("movflags", JsonValue("+faststart"));
  n.options = JsonValue(std::move(o));
  return n;
}

Result<GraphSpec> MixTemplate::Graph(const LayoutSpec& layout, const MixOptions& options,
                                     const std::vector<MixSpec>& members, const MixResultSpec& result,
                                     std::string_view graph_id) {
  if (members.empty()) return Status::InvalidArgument("mix needs at least one member");
  if (members.size() > static_cast<std::size_t>(kMaxMembers)) {
    return Status::InvalidArgument("mix supports at most " + std::to_string(kMaxMembers) + " members");
  }
  if (layout.width <= 0 || layout.height <= 0 || (layout.width % 2) != 0 || (layout.height % 2) != 0) {
    return Status::InvalidArgument("layout width/height must be positive and even");
  }
  if (layout.fps <= 0) return Status::InvalidArgument("layout fps must be > 0");
  if (result.output_path.empty()) return Status::InvalidArgument("output_path is required");

  GraphSpec g{std::string(graph_id)};
  const LayoutSpec effective = EffectiveLayout(layout, members);
  if (Status s = g.AddNode(ComposeNode(effective, options)); !s.ok()) return s;
  if (Status s = g.AddNode(EncodeNode(result)); !s.ok()) return s;
  if (Status s = g.AddNode(MuxNode(result)); !s.ok()) return s;
  if (Status s = g.AddEdge(Edge("e.compose-venc", Port("compose.out"), Port("venc.in"), Cap(options))); !s.ok()) return s;
  if (Status s = g.AddEdge(Edge("e.venc-mux", Port("venc.out"), Port("mux.video"), Cap(options))); !s.ok()) return s;
  if (options.audio) {
    if (Status s = g.AddNode(MixNode(effective, options)); !s.ok()) return s;
    if (Status s = g.AddNode(AudioEncodeNode(result)); !s.ok()) return s;
    if (Status s = g.AddEdge(Edge("e.amix-aenc", Port("amix.out"), Port("aenc.in"), Cap(options))); !s.ok()) return s;
    if (Status s = g.AddEdge(Edge("e.aenc-mux", Port("aenc.out"), Port("mux.audio"), Cap(options))); !s.ok()) return s;
  }

  for (std::size_t i = 0; i < members.size(); ++i) {
    const MixSpec& m = members[i];
    if (Status s = ValidateMember(m); !s.ok()) return s;
    const std::string port = MemberPort(static_cast<int>(i));
    const MixNodeIds ids;
    const std::string dec = MemberNodeId(m.member_id, "dec");
    if (Status s = g.AddNode(MemberDecodeNode(m)); !s.ok()) return s;
    NodeSpec vdec;
    vdec.id = dec;
    vdec.op = Key(kOpVideoDecode);
    if (Status s = g.AddNode(std::move(vdec)); !s.ok()) return s;
    if (Status s = g.AddEdge(Edge(ids.MemberEntryEdge(m.member_id), PortRef{ids.MemberSource(m.member_id), "video"},
                                  PortRef{dec, "in"}, Cap(options)));
        !s.ok()) {
      return s;
    }
    if (Status s = g.AddEdge(Edge(ids.MemberComposeEdge(m.member_id, port), PortRef{dec, "out"}, PortRef{"compose", port},
                                  Cap(options)));
        !s.ok()) {
      return s;
    }
    if (!options.audio) continue;
    const std::string adec = MemberNodeId(m.member_id, "adec");
    if (Status s = g.AddNode(MemberAudioDecodeNode(m)); !s.ok()) return s;
    NodeSpec adec_node;
    adec_node.id = adec;
    adec_node.op = Key(kOpAudioDecode);
    if (Status s = g.AddNode(std::move(adec_node)); !s.ok()) return s;
    if (Status s = g.AddEdge(Edge(ids.MemberAudioEntryEdge(m.member_id), PortRef{ids.MemberAudioSource(m.member_id), "audio"},
                                  PortRef{adec, "in"}, Cap(options)));
        !s.ok()) {
      return s;
    }
    if (Status s = g.AddEdge(Edge(ids.MemberAudioEntryEdge(m.member_id) + ".mix", PortRef{adec, "out"},
                                  PortRef{"amix", port}, Cap(options)));
        !s.ok()) {
      return s;
    }
  }
  return g;
}

Result<MutationPatch> MixTemplate::AddMember(const MixSpec& spec, int port_index, const LayoutSpec& layout,
                                             const MixOptions& options) {
  (void)layout;
  if (Status s = ValidateMember(spec); !s.ok()) return s;
  if (port_index < 0 || port_index >= kMaxMembers) return Status::InvalidArgument("member port index out of range");
  const std::string port = MemberPort(port_index);
  const MixNodeIds ids;
  const std::string dec = MemberNodeId(spec.member_id, "dec");
  const std::string adec = MemberNodeId(spec.member_id, "adec");
  Mutation m;
  m.AddNode(MemberDecodeNode(spec));
  NodeSpec vdec;
  vdec.id = dec;
  vdec.op = Key(kOpVideoDecode);
  m.AddNode(std::move(vdec));
  EdgeOptions eo;
  eo.queue.capacity = Cap(options);
  eo.queue.policy = DropPolicy::kBlock;
  eo.id = ids.MemberEntryEdge(spec.member_id);
  m.AddEdge(PortRef{ids.MemberSource(spec.member_id), "video"}, PortRef{dec, "in"}, eo);
  eo.id = ids.MemberComposeEdge(spec.member_id, port);
  m.AddEdge(PortRef{dec, "out"}, PortRef{"compose", port}, eo);
  if (options.audio) {
    m.AddNode(MemberAudioDecodeNode(spec));
    NodeSpec adec_node;
    adec_node.id = adec;
    adec_node.op = Key(kOpAudioDecode);
    m.AddNode(std::move(adec_node));
    eo.id = ids.MemberAudioEntryEdge(spec.member_id);
    m.AddEdge(PortRef{ids.MemberAudioSource(spec.member_id), "audio"}, PortRef{adec, "in"}, eo);
    eo.id = ids.MemberAudioEntryEdge(spec.member_id) + ".mix";
    m.AddEdge(PortRef{adec, "out"}, PortRef{"amix", port}, eo);
  }
  return m.patch();
}

Result<MutationPatch> MixTemplate::RemoveMember(const MixSpec& spec, int port_index, const MixOptions& options,
                                                RemovePolicy policy, bool drain) {
  if (port_index < 0 || port_index >= kMaxMembers) return Status::InvalidArgument("member port index out of range");
  (void)drain;  // |policy| already encodes drain (kDrain) vs immediate (kFast)
  // A member branch is rooted at its own demux source and ends at the shared
  // compose/amix input port, so it is removed source-down with RemoveNode
  // (RemoveBranch models a branch hanging *below* a shared node and does not
  // apply). RemoveNode drops the incident edges with the node; the member's
  // compose/amix ports are optional, so the remaining graph stays valid.
  const MixNodeIds ids;
  Mutation m;
  m.SetRemovePolicy(policy);
  m.RemoveNode(ids.MemberSource(spec.member_id), RemoveOptions{policy});
  m.RemoveNode(ids.MemberDecode(spec.member_id), RemoveOptions{policy});
  if (options.audio) {
    m.RemoveNode(ids.MemberAudioSource(spec.member_id), RemoveOptions{policy});
    m.RemoveNode(ids.MemberAudioDecode(spec.member_id), RemoveOptions{policy});
  }
  return m.patch();
}

}  // namespace ge::media
