#include <ge/media/operators.h>

#include "media/ffmpeg.h"
#include "media/operators_internal.h"

namespace ge::media {

namespace {

PortCapability Port(std::string_view name, PortDirection dir, std::string_view tag, bool required = true,
                    PortCardinality card = PortCardinality::kSingle) {
  PortCapability p;
  p.name = std::string(name);
  p.direction = dir;
  p.type_tag = std::string(tag);
  p.required = required;
  p.cardinality = card;
  p.memory.kinds = std::vector<MemoryKind>{MemoryKind::kHost};
  return p;
}

PortCapability VideoPort(std::string_view name, PortDirection dir, FormatSet pixel_formats,
                         PortCardinality card = PortCardinality::kSingle) {
  PortCapability p = Port(name, dir, kTagVideoFrame, true, card);
  p.video = VideoConstraints{};
  p.video->pixel_formats = std::move(pixel_formats);
  return p;
}

PortCapability AudioPort(std::string_view name, PortDirection dir, FormatSet sample_formats,
                         PortCardinality card = PortCardinality::kSingle) {
  PortCapability p = Port(name, dir, kTagAudioFrame, true, card);
  p.audio = AudioConstraints{};
  p.audio->sample_formats = std::move(sample_formats);
  return p;
}

CapabilityDescriptor Base(std::string_view key, std::string description) {
  CapabilityDescriptor d;
  d.op = *OperatorKey::Parse(key);
  d.description = std::move(description);
  d.execution.devices = {DeviceKind::kCpu};
  d.execution.stateful = true;
  d.execution.async = false;
  d.execution.max_parallelism = 1;
  return d;
}

JsonValue Schema(std::initializer_list<std::pair<const char*, const char*>> props) {
  JsonObject p;
  for (const auto& [name, type] : props) p.emplace(name, JsonValue(JsonObject{{"type", JsonValue(type)}}));
  return JsonValue(JsonObject{{"type", JsonValue("object")}, {"properties", JsonValue(std::move(p))}});
}

}  // namespace

const FormatSet& DecodedPixelFormats() {
  static const FormatSet kFormats{"yuv420p", "nv12", "yuv422p", "yuv444p", "yuvj420p"};
  return kFormats;
}

const FormatSet& EncoderPixelFormats() {
  static const FormatSet kFormats{"yuv420p", "nv12"};
  return kFormats;
}

CapabilityDescriptor MediaDemuxCapability() {
  CapabilityDescriptor d = Base(kOpMediaDemux, "libavformat demuxer source");
  d.outputs = {Port("video", PortDirection::kOutput, kTagEncodedVideo, true, PortCardinality::kMulti),
               Port("audio", PortDirection::kOutput, kTagEncodedAudio, true, PortCardinality::kMulti)};
  for (PortCapability& p : d.outputs) p.dynamic_consumers = true;
  d.parameters.schema = Schema({{"input_path", "string"}, {"realtime", "boolean"}, {"loop", "boolean"},
                                {"start_ms", "integer"}, {"end_ms", "integer"}});
  return d;
}

CapabilityDescriptor VideoDecodeCapability() {
  CapabilityDescriptor d = Base(kOpVideoDecode, "libavcodec video decoder");
  d.inputs = {Port("in", PortDirection::kInput, kTagEncodedVideo)};
  PortCapability out = VideoPort("out", PortDirection::kOutput, DecodedPixelFormats(), PortCardinality::kMulti);
  out.dynamic_consumers = true;
  d.outputs = {out};
  d.parameters.schema = Schema({{"backend", "string"}});
  return d;
}

CapabilityDescriptor AudioDecodeCapability() {
  CapabilityDescriptor d = Base(kOpAudioDecode, "libavcodec audio decoder");
  d.inputs = {Port("in", PortDirection::kInput, kTagEncodedAudio)};
  PortCapability out = AudioPort("out", PortDirection::kOutput, {"fltp", "s16", "flt", "s16p"}, PortCardinality::kMulti);
  out.dynamic_consumers = true;
  d.outputs = {out};
  d.parameters.schema = Schema({{"backend", "string"}});
  return d;
}

CapabilityDescriptor VideoScaleCapability() {
  CapabilityDescriptor d = Base(kOpVideoScale, "libavfilter scale (+drawbox watermark)");
  d.inputs = {VideoPort("in", PortDirection::kInput, DecodedPixelFormats())};
  d.outputs = {VideoPort("out", PortDirection::kOutput, EncoderPixelFormats(), PortCardinality::kMulti)};
  d.parameters.hot_updatable = {"watermark"};
  d.parameters.schema = Schema({{"width", "integer"}, {"height", "integer"}, {"watermark", "object"}});
  return d;
}

CapabilityDescriptor VideoFilterCapability() {
  CapabilityDescriptor d = Base(kOpVideoFilter, "libavfilter chain (drawtext, overlay, ...) between two VideoFrame ports");
  d.inputs = {VideoPort("in", PortDirection::kInput, DecodedPixelFormats())};
  d.outputs = {VideoPort("out", PortDirection::kOutput, EncoderPixelFormats(), PortCardinality::kMulti)};
  d.parameters.hot_updatable = {"filter"};
  d.parameters.schema = JsonValue(JsonObject{{"type", JsonValue("object")},
                                             {"properties", JsonValue(JsonObject{{"filter", JsonValue(JsonObject{{"type", JsonValue("string")}})}})},
                                             {"required", JsonValue(JsonArray{JsonValue("filter")})}});
  return d;
}

CapabilityDescriptor VideoConvertCapability() {
  CapabilityDescriptor d = Base(kOpVideoConvert, "swscale pixel format conversion");
  d.inputs = {VideoPort("in", PortDirection::kInput, DecodedPixelFormats())};
  d.outputs = {VideoPort("out", PortDirection::kOutput, DecodedPixelFormats(), PortCardinality::kMulti)};
  return d;
}

CapabilityDescriptor VideoEncodeCapability() {
  CapabilityDescriptor d = Base(kOpVideoEncode, "libavcodec video encoder");
  d.inputs = {VideoPort("in", PortDirection::kInput, EncoderPixelFormats())};
  d.outputs = {Port("out", PortDirection::kOutput, kTagEncodedVideo, true, PortCardinality::kMulti)};
  d.parameters.hot_updatable = {"bitrate_kbps", "gop", "force_idr"};
  d.parameters.schema = Schema({{"codec", "string"},
                                {"backend", "string"},
                                {"bitrate_kbps", "integer"},
                                {"gop", "integer"},
                                {"fps", "integer"},
                                {"preset", "string"},
                                {"force_idr", "boolean"},
                                {"segment_duration_ms", "integer"}});
  d.events.emits = {std::string(kEventMediaFormatChanged)};
  return d;
}

CapabilityDescriptor AudioEncodeCapability() {
  CapabilityDescriptor d = Base(kOpAudioEncode, "libavcodec AAC encoder");
  d.inputs = {AudioPort("in", PortDirection::kInput, {"fltp", "s16", "flt", "s16p"})};
  PortCapability out = Port("out", PortDirection::kOutput, kTagEncodedAudio, true, PortCardinality::kMulti);
  out.dynamic_consumers = true;
  d.outputs = {out};
  d.parameters.schema = Schema({{"bitrate_kbps", "integer"}, {"sample_rate", "integer"}, {"channels", "integer"}});
  return d;
}

CapabilityDescriptor MediaMuxCapability() {
  CapabilityDescriptor d = Base(kOpMediaMux, "libavformat muxer sink (flv|mp4)");
  d.inputs = {Port("video", PortDirection::kInput, kTagEncodedVideo, true),
              Port("audio", PortDirection::kInput, kTagEncodedAudio, false)};
  for (PortCapability& p : d.inputs) p.sync = std::vector<SyncPolicy>{SyncPolicy::kAny};
  d.parameters.schema = Schema({{"output_path", "string"}, {"output_pattern", "string"}, {"container", "string"},
                                {"movflags", "string"}, {"align_start", "boolean"}});
  d.events.emits = {std::string(kEventMediaSegment)};
  return d;
}

void RegisterMediaOperators(BuiltinOperatorFactory& factory) {
  factory.Register(MediaDemuxCapability(), &MakeMediaDemux);
  factory.Register(VideoDecodeCapability(), &MakeVideoDecode);
  factory.Register(AudioDecodeCapability(), &MakeAudioDecode);
  factory.Register(VideoScaleCapability(), &MakeVideoScale);
  factory.Register(VideoConvertCapability(), &MakeVideoConvert);
  factory.Register(VideoFilterCapability(), &MakeVideoFilter);
  factory.Register(VideoEncodeCapability(), &MakeVideoEncode);
  factory.Register(AudioEncodeCapability(), &MakeAudioEncode);
  factory.Register(MediaMuxCapability(), &MakeMediaMux);
}

std::shared_ptr<BuiltinOperatorFactory> MakeMediaOperatorFactory() {
  auto f = std::make_shared<BuiltinOperatorFactory>();
  RegisterMediaOperators(*f);
  return f;
}

void SetFfmpegLogLevel(FfmpegLogLevel level) noexcept {
  switch (level) {
    case FfmpegLogLevel::kQuiet: av_log_set_level(AV_LOG_QUIET); break;
    case FfmpegLogLevel::kError: av_log_set_level(AV_LOG_ERROR); break;
    case FfmpegLogLevel::kWarning: av_log_set_level(AV_LOG_WARNING); break;
    case FfmpegLogLevel::kInfo: av_log_set_level(AV_LOG_INFO); break;
  }
}

std::string PreferredVideoEncoder(std::string_view codec) {
  const AVCodec* c = FindVideoEncoder(codec, CodecBackend::kSoftware);
  return c == nullptr ? std::string() : std::string(c->name);
}

// ---------------------------------------------------------------------------
// Shared helpers (operators_internal.h)
// ---------------------------------------------------------------------------

const AVCodec* FindVideoEncoder(std::string_view codec, CodecBackend backend) {
  if (backend == CodecBackend::kVideoToolbox) {
    return avcodec_find_encoder_by_name(codec == "hevc" ? "hevc_videotoolbox" : "h264_videotoolbox");
  }
  if (backend == CodecBackend::kNvidia) {
    return avcodec_find_encoder_by_name(codec == "hevc" ? "hevc_nvenc" : "h264_nvenc");
  }
  if (codec == "h264" || codec.empty()) {
    for (const char* name : {"libx264", "libopenh264"}) {
      if (const AVCodec* c = avcodec_find_encoder_by_name(name)) return c;
    }
    return avcodec_find_encoder(AV_CODEC_ID_MPEG4);  // always built in
  }
  if (codec == "hevc") {
    if (const AVCodec* c = avcodec_find_encoder_by_name("libx265")) return c;
    return nullptr;
  }
  if (codec == "mpeg4") return avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  return avcodec_find_encoder_by_name(std::string(codec).c_str());
}

Status RejectNonSoftwareBackend(const JsonValue& options, std::string_view node) {
  const std::string text = options.GetString("backend").value_or("software");
  const std::optional<CodecBackend> backend = ParseCodecBackend(text);
  if (!backend) return Status::InvalidArgument("node '" + std::string(node) + "': unknown backend '" + text + "'");
  if (*backend != CodecBackend::kSoftware) {
    return Status::ResourceExhausted("node '" + std::string(node) + "': backend '" + text +
                                     "' is not available in this build (software only)");
  }
  return Status::Ok();
}

}  // namespace ge::media
