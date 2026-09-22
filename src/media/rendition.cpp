#include <ge/media/rendition.h>

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

}  // namespace

JsonValue WatermarkSpec::ToJson() const {
  return JsonValue(JsonObject{{"enabled", JsonValue(true)},
                              {"x", JsonValue(x)},
                              {"y", JsonValue(y)},
                              {"w", JsonValue(w)},
                              {"h", JsonValue(h)},
                              {"color", JsonValue(color)}});
}

RenditionNodeIds RenditionTemplate::Ids(std::string_view rendition_id) {
  const std::string base = "r." + std::string(rendition_id);
  RenditionNodeIds ids;
  ids.scale = base + ".scale";
  ids.venc = base + ".venc";
  ids.mux = base + ".mux";
  ids.video_entry_edge = base + ".e.video";
  ids.audio_entry_edge = base + ".e.audio";
  ids.scale_venc_edge = base + ".e.scale-venc";
  ids.venc_mux_edge = base + ".e.venc-mux";
  return ids;
}

std::string RenditionNodeIds::Filter(std::string_view filter_id) const {
  // scale is "r.<id>.scale"; the filter shares the "r.<id>." prefix.
  return scale.substr(0, scale.size() - 5) + "f." + std::string(filter_id);
}

Status RenditionTemplate::ValidateSpec(const RenditionSpec& spec) {
  if (!IsValidStableId(spec.id) || spec.id.size() > 64) {
    return Status::InvalidArgument("rendition id '" + spec.id + "' is not a valid stable id");
  }
  if (spec.width <= 0 || spec.height <= 0 || (spec.width % 2) != 0 || (spec.height % 2) != 0) {
    return Status::InvalidArgument("rendition '" + spec.id + "': width/height must be positive and even");
  }
  if (spec.bitrate_kbps <= 0) return Status::InvalidArgument("rendition '" + spec.id + "': bitrate_kbps must be > 0");
  if (spec.gop <= 0) return Status::InvalidArgument("rendition '" + spec.id + "': gop must be > 0");
  if (spec.output_path.empty()) return Status::InvalidArgument("rendition '" + spec.id + "': output_path is required");
  if (spec.container != "flv" && spec.container != "mp4" && spec.container != "matroska") {
    return Status::InvalidArgument("rendition '" + spec.id + "': container must be flv|mp4|matroska");
  }
  return Status::Ok();
}

NodeSpec RenditionTemplate::ScaleNode(const RenditionSpec& spec) {
  NodeSpec n;
  n.id = Ids(spec.id).scale;
  n.op = Key(kOpVideoScale);
  JsonObject o{{"width", JsonValue(spec.width)}, {"height", JsonValue(spec.height)}};
  if (spec.watermark) o.emplace("watermark", spec.watermark->ToJson());
  n.options = JsonValue(std::move(o));
  return n;
}

NodeSpec RenditionTemplate::EncodeNode(const RenditionSpec& spec) {
  NodeSpec n;
  n.id = Ids(spec.id).venc;
  n.op = Key(kOpVideoEncode);
  JsonObject o{{"codec", JsonValue(spec.codec)},
               {"bitrate_kbps", JsonValue(spec.bitrate_kbps)},
               {"gop", JsonValue(spec.gop)},
               {"preset", JsonValue(spec.preset)}};
  if (spec.fps > 0) o.emplace("fps", JsonValue(spec.fps));
  n.options = JsonValue(std::move(o));
  return n;
}

NodeSpec RenditionTemplate::MuxNode(const RenditionSpec& spec) {
  NodeSpec n;
  n.id = Ids(spec.id).mux;
  n.op = Key(kOpMediaMux);
  JsonObject o{{"output_path", JsonValue(spec.output_path)}, {"container", JsonValue(spec.container)}};
  if (spec.container == "mp4") o.emplace("movflags", JsonValue("+faststart"));
  n.options = JsonValue(std::move(o));
  return n;
}

Result<GraphSpec> RenditionTemplate::BaseGraph(const BaseOptions& options, std::string_view graph_id) {
  if (options.input_path.empty()) return Status::InvalidArgument("input_path is required");
  GraphSpec g{std::string(graph_id)};
  NodeSpec demux;
  demux.id = kDemux;
  demux.op = Key(kOpMediaDemux);
  demux.options = JsonValue(JsonObject{{"input_path", JsonValue(options.input_path)},
                                       {"realtime", JsonValue(options.realtime)},
                                       {"loop", JsonValue(options.loop)}});
  if (Status s = g.AddNode(std::move(demux)); !s.ok()) return s;
  NodeSpec vdec;
  vdec.id = kVideoDecode;
  vdec.op = Key(kOpVideoDecode);
  if (Status s = g.AddNode(std::move(vdec)); !s.ok()) return s;
  if (Status s = g.AddEdge(Edge("e.demux-vdec", Port("demux.video"), Port("vdec.in"), options.queue_capacity)); !s.ok()) return s;
  if (options.audio) {
    NodeSpec adec;
    adec.id = kAudioDecode;
    adec.op = Key(kOpAudioDecode);
    if (Status s = g.AddNode(std::move(adec)); !s.ok()) return s;
    NodeSpec aenc;
    aenc.id = kAudioEncode;
    aenc.op = Key(kOpAudioEncode);
    aenc.options = JsonValue(JsonObject{{"bitrate_kbps", JsonValue(options.audio_bitrate_kbps)}});
    if (Status s = g.AddNode(std::move(aenc)); !s.ok()) return s;
    if (Status s = g.AddEdge(Edge("e.demux-adec", Port("demux.audio"), Port("adec.in"), options.queue_capacity)); !s.ok()) return s;
    if (Status s = g.AddEdge(Edge("e.adec-aenc", Port("adec.out"), Port("aenc.in"), options.queue_capacity)); !s.ok()) return s;
  }
  return g;
}

Result<GraphSpec> RenditionTemplate::Graph(const BaseOptions& options, const std::vector<RenditionSpec>& renditions,
                                           std::string_view graph_id) {
  Result<GraphSpec> g = BaseGraph(options, graph_id);
  if (!g.ok()) return g;
  for (const RenditionSpec& r : renditions) {
    if (Status s = ValidateSpec(r); !s.ok()) return s;
    const RenditionNodeIds ids = Ids(r.id);
    if (Status s = g->AddNode(ScaleNode(r)); !s.ok()) return s;
    if (Status s = g->AddNode(EncodeNode(r)); !s.ok()) return s;
    if (Status s = g->AddNode(MuxNode(r)); !s.ok()) return s;
    if (Status s = g->AddEdge(Edge(ids.video_entry_edge, Port(options.video_source_port), PortRef{ids.scale, "in"}, r.queue_capacity)); !s.ok()) return s;
    if (Status s = g->AddEdge(Edge(ids.scale_venc_edge, PortRef{ids.scale, "out"}, PortRef{ids.venc, "in"}, r.queue_capacity)); !s.ok()) return s;
    if (Status s = g->AddEdge(Edge(ids.venc_mux_edge, PortRef{ids.venc, "out"}, PortRef{ids.mux, "video"}, r.queue_capacity)); !s.ok()) return s;
    if (r.audio && options.audio) {
      if (Status s = g->AddEdge(Edge(ids.audio_entry_edge, Port(options.audio_source_port), PortRef{ids.mux, "audio"}, r.queue_capacity)); !s.ok()) return s;
    }
  }
  return g;
}

Result<MutationPatch> RenditionTemplate::AddRendition(const RenditionSpec& spec, const BaseOptions& base) {
  if (Status s = ValidateSpec(spec); !s.ok()) return s;
  const RenditionNodeIds ids = Ids(spec.id);
  Mutation m;
  m.AddNode(ScaleNode(spec));
  m.AddNode(EncodeNode(spec));
  m.AddNode(MuxNode(spec));
  EdgeOptions eo;
  eo.queue.capacity = spec.queue_capacity;
  eo.queue.policy = DropPolicy::kBlock;
  eo.id = ids.video_entry_edge;
  m.AddEdge(Port(base.video_source_port), PortRef{ids.scale, "in"}, eo);
  eo.id = ids.scale_venc_edge;
  m.AddEdge(PortRef{ids.scale, "out"}, PortRef{ids.venc, "in"}, eo);
  eo.id = ids.venc_mux_edge;
  m.AddEdge(PortRef{ids.venc, "out"}, PortRef{ids.mux, "video"}, eo);
  if (spec.audio && base.audio) {
    eo.id = ids.audio_entry_edge;
    m.AddEdge(Port(base.audio_source_port), PortRef{ids.mux, "audio"}, eo);
  }
  return m.patch();
}

MutationPatch RenditionTemplate::RemoveRendition(std::string_view rendition_id, RemovePolicy policy, bool audio,
                                                 const BaseOptions& base) {
  const RenditionNodeIds ids = Ids(rendition_id);
  std::vector<std::string> entries{ids.video_entry_edge};
  if (audio && base.audio) entries.push_back(ids.audio_entry_edge);
  Mutation m;
  m.SetRemovePolicy(policy);
  m.RemoveBranch(std::move(entries), BranchSelection{{ids.scale, ids.venc, ids.mux}}, RemoveOptions{policy});
  return m.patch();
}

Status RenditionTemplate::ValidateFilter(const FilterSpec& filter) {
  if (!IsValidStableId(filter.id) || filter.id.size() > 64) {
    return Status::InvalidArgument("filter id '" + filter.id + "' is not a valid stable id");
  }
  if (filter.chain.empty()) return Status::InvalidArgument("filter '" + filter.id + "': chain is required");
  return ValidateFilterChain(filter.chain);
}

NodeSpec RenditionTemplate::FilterNode(std::string_view rendition_id, const FilterSpec& filter) {
  NodeSpec n;
  n.id = Ids(rendition_id).Filter(filter.id);
  n.op = Key(kOpVideoFilter);
  n.options = JsonValue(JsonObject{{"filter", JsonValue(filter.chain)}});
  return n;
}

Result<std::string> RenditionTemplate::FilterEntryEdge(const GraphSpec& current, std::string_view rendition_id) {
  const RenditionNodeIds ids = Ids(rendition_id);
  if (current.FindNode(ids.venc) == nullptr) {
    return Status::NotFound("rendition '" + std::string(rendition_id) + "' is not in the graph");
  }
  const std::vector<const EdgeSpec*> in = current.EdgesToPort(PortRef{ids.venc, "in"});
  if (in.size() != 1) {
    return Status::GraphInvalid("rendition '" + std::string(rendition_id) + "': expected exactly one edge into " + ids.venc +
                                ".in, found " + std::to_string(in.size()));
  }
  return in.front()->id;
}

Result<MutationPatch> RenditionTemplate::InsertFilter(const GraphSpec& current, std::string_view rendition_id,
                                                      const FilterSpec& filter) {
  if (Status s = ValidateFilter(filter); !s.ok()) return s;
  const NodeSpec node = FilterNode(rendition_id, filter);
  if (current.FindNode(node.id) != nullptr) {
    return Status::AlreadyExists("filter '" + filter.id + "' exists in rendition '" + std::string(rendition_id) + "'");
  }
  Result<std::string> edge = FilterEntryEdge(current, rendition_id);
  if (!edge.ok()) return edge.status();
  Mutation m;
  m.InsertChain(std::move(*edge), {node}, ChainPorts{.chain_input = "in", .chain_output = "out"});
  return m.patch();
}

MutationPatch RenditionTemplate::RemoveFilter(std::string_view rendition_id, std::string_view filter_id,
                                              RemovePolicy policy) {
  Mutation m;
  m.SetRemovePolicy(policy);
  m.RemoveChain({Ids(rendition_id).Filter(filter_id)}, RemoveChainOptions{.bypass = true, .remove_policy = policy});
  return m.patch();
}

JsonValue RenditionTemplate::FilterParameters(std::string_view chain) {
  return JsonValue(JsonObject{{"filter", JsonValue(std::string(chain))}});
}

JsonValue RenditionTemplate::EncoderParameters(const HotUpdate& u) {
  JsonObject o;
  if (u.bitrate_kbps) o.emplace("bitrate_kbps", JsonValue(*u.bitrate_kbps));
  if (u.gop) o.emplace("gop", JsonValue(*u.gop));
  if (u.force_idr) o.emplace("force_idr", JsonValue(true));
  return JsonValue(std::move(o));
}

JsonValue RenditionTemplate::ScaleParameters(const HotUpdate& u) {
  JsonObject o;
  if (u.watermark) {
    o.emplace("watermark", u.watermark->ToJson());
  } else if (u.clear_watermark) {
    o.emplace("watermark", JsonValue(JsonObject{{"enabled", JsonValue(false)}}));
  }
  return JsonValue(std::move(o));
}

}  // namespace ge::media
