#include "media/frame_internal.h"

#include <array>
#include <cstring>

namespace ge::media {

// ---------------------------------------------------------------------------
// Payload wrapping. The Buffer's data points at the AVFrame/AVPacket
// itself; |deleter_context| tags the class so FrameOf/PacketOf can check.
// ---------------------------------------------------------------------------

namespace {

int kFrameTag = 0;
int kPacketTag = 0;

void FreeFrame(Buffer* b, void*) {
  auto* f = static_cast<AVFrame*>(b->data);
  av_frame_free(&f);
}

void FreePacket(Buffer* b, void*) {
  auto* p = static_cast<AVPacket*>(b->data);
  av_packet_free(&p);
}

const Buffer* MediaBuffer(const Packet& p, const void* tag) noexcept {
  const Buffer* b = p.payload.get();
  if (b == nullptr || b->data == nullptr) return nullptr;
  return b->deleter_context == tag ? b : nullptr;
}

}  // namespace

BufferRef WrapFrame(ff::FramePtr frame) {
  if (!frame) return nullptr;
  AVFrame* raw = frame.release();
  return WrapExternalBuffer(raw, sizeof(AVFrame), MemoryKind::kHost, -1, &FreeFrame, &kFrameTag);
}

BufferRef WrapPacket(ff::PacketPtr packet) {
  if (!packet) return nullptr;
  AVPacket* raw = packet.release();
  return WrapExternalBuffer(raw, sizeof(AVPacket), MemoryKind::kHost, -1, &FreePacket, &kPacketTag);
}

const AVFrame* FrameOf(const Packet& p) noexcept {
  const Buffer* b = MediaBuffer(p, &kFrameTag);
  return b == nullptr ? nullptr : static_cast<const AVFrame*>(b->data);
}

const AVPacket* PacketOf(const Packet& p) noexcept {
  const Buffer* b = MediaBuffer(p, &kPacketTag);
  return b == nullptr ? nullptr : static_cast<const AVPacket*>(b->data);
}

bool IsFramePayload(const Buffer& b) noexcept { return b.deleter_context == &kFrameTag; }
bool IsPacketPayload(const Buffer& b) noexcept { return b.deleter_context == &kPacketTag; }

// ---------------------------------------------------------------------------
// MediaFormat
// ---------------------------------------------------------------------------

namespace {

JsonValue RationalJson(Rational r) {
  return JsonValue(JsonArray{JsonValue(r.num), JsonValue(r.den)});
}

std::optional<Rational> RationalFrom(const JsonValue* v) {
  if (v == nullptr || !v->is_array() || v->as_array().size() != 2) return std::nullopt;
  const JsonArray& a = v->as_array();
  if (!a[0].is_number() || !a[1].is_number()) return std::nullopt;
  return Rational{static_cast<std::int32_t>(a[0].as_integer()), static_cast<std::int32_t>(a[1].as_integer())};
}

std::int32_t I32(const JsonValue& v, std::string_view key, std::int32_t def = 0) {
  return static_cast<std::int32_t>(v.GetInteger(key).value_or(def));
}

}  // namespace

JsonValue MediaFormat::ToJson() const {
  JsonObject o;
  o.emplace("kind", JsonValue(kind == MediaKind::kVideo ? "video" : "audio"));
  o.emplace("time_base", RationalJson(time_base));
  if (!codec.empty()) o.emplace("codec", JsonValue(codec));
  if (kind == MediaKind::kVideo) {
    o.emplace("pixel_format", JsonValue(pixel_format));
    o.emplace("width", JsonValue(width));
    o.emplace("height", JsonValue(height));
    o.emplace("frame_rate", RationalJson(frame_rate));
    o.emplace("sar", RationalJson(sample_aspect_ratio));
  } else {
    o.emplace("sample_format", JsonValue(sample_format));
    o.emplace("sample_rate", JsonValue(sample_rate));
    o.emplace("channels", JsonValue(channels));
    o.emplace("channel_layout", JsonValue(channel_layout));
  }
  return JsonValue(std::move(o));
}

Result<MediaFormat> MediaFormat::FromJson(const JsonValue& v) {
  if (!v.is_object()) return Status::InvalidArgument("media format must be an object");
  MediaFormat f;
  const std::string kind = v.GetString("kind").value_or("");
  if (kind == "video") {
    f.kind = MediaKind::kVideo;
  } else if (kind == "audio") {
    f.kind = MediaKind::kAudio;
  } else {
    return Status::InvalidArgument("media format kind must be video|audio");
  }
  if (const auto tb = RationalFrom(v.Find("time_base"))) f.time_base = *tb;
  f.codec = v.GetString("codec").value_or("");
  if (f.kind == MediaKind::kVideo) {
    f.pixel_format = v.GetString("pixel_format").value_or("");
    f.width = I32(v, "width");
    f.height = I32(v, "height");
    if (const auto fr = RationalFrom(v.Find("frame_rate"))) f.frame_rate = *fr;
    if (const auto sar = RationalFrom(v.Find("sar"))) f.sample_aspect_ratio = *sar;
  } else {
    f.sample_format = v.GetString("sample_format").value_or("");
    f.sample_rate = I32(v, "sample_rate");
    f.channels = I32(v, "channels");
    f.channel_layout = v.GetString("channel_layout").value_or("");
  }
  return f;
}

std::optional<MediaFormat> MediaFormat::FromPacket(const Packet& p) {
  const std::string* text = p.meta().Get(kMetaFormat);
  if (text == nullptr) return std::nullopt;
  JsonParseResult parsed = ParseJson(*text);
  if (!parsed.ok()) return std::nullopt;
  Result<MediaFormat> f = FromJson(*parsed.value);
  if (!f.ok()) return std::nullopt;
  return *f;
}

void SetFormat(Packet* packet, const MediaFormat& format, const JsonValue* codecpar) {
  auto m = std::make_shared<Metadata>(packet->meta());
  m->Set(kMetaFormat, format.Serialize());
  if (codecpar != nullptr) m->Set(kMetaCodecPar, codecpar->Serialize());
  packet->metadata = std::move(m);
}

const std::string* CodecParJson(const Packet& p) { return p.meta().Get(kMetaCodecPar); }

bool IsKnownPixelFormat(std::string_view name) {
  return ff::PixelFormatFromName(name) != AV_PIX_FMT_NONE;
}

bool IsKnownSampleFormat(std::string_view name) {
  return ff::SampleFormatFromName(name) != AV_SAMPLE_FMT_NONE;
}

MediaFormat FormatOfFrame(const AVFrame& f, MediaKind kind, AVRational time_base) {
  MediaFormat m;
  m.kind = kind;
  m.time_base = FromAv(time_base);
  if (kind == MediaKind::kVideo) {
    m.pixel_format = ff::PixelFormatName(static_cast<AVPixelFormat>(f.format));
    m.width = f.width;
    m.height = f.height;
    m.sample_aspect_ratio = f.sample_aspect_ratio.num == 0 ? Rational{1, 1} : FromAv(f.sample_aspect_ratio);
  } else {
    m.sample_format = ff::SampleFormatName(static_cast<AVSampleFormat>(f.format));
    m.sample_rate = f.sample_rate;
    m.channels = f.ch_layout.nb_channels;
    char buf[64] = {};
    av_channel_layout_describe(&f.ch_layout, buf, sizeof(buf));
    m.channel_layout = buf;
  }
  return m;
}

MediaFormat FormatOfCodecPar(const AVCodecParameters& par, AVRational time_base) {
  MediaFormat m;
  m.time_base = FromAv(time_base);
  const char* name = avcodec_get_name(par.codec_id);
  m.codec = name == nullptr ? "" : name;
  if (par.codec_type == AVMEDIA_TYPE_VIDEO) {
    m.kind = MediaKind::kVideo;
    m.pixel_format = ff::PixelFormatName(static_cast<AVPixelFormat>(par.format));
    m.width = par.width;
    m.height = par.height;
    m.frame_rate = FromAv(par.framerate);
    m.sample_aspect_ratio = par.sample_aspect_ratio.num == 0 ? Rational{1, 1} : FromAv(par.sample_aspect_ratio);
  } else {
    m.kind = MediaKind::kAudio;
    m.sample_format = ff::SampleFormatName(static_cast<AVSampleFormat>(par.format));
    m.sample_rate = par.sample_rate;
    m.channels = par.ch_layout.nb_channels;
    char buf[64] = {};
    av_channel_layout_describe(&par.ch_layout, buf, sizeof(buf));
    m.channel_layout = buf;
  }
  return m;
}

// ---------------------------------------------------------------------------
// codecpar <-> JSON
// ---------------------------------------------------------------------------

namespace {
constexpr std::string_view kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string Base64Encode(const std::uint8_t* data, std::size_t size) {
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (std::size_t i = 0; i < size; i += 3) {
    std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
    if (i + 1 < size) n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
    if (i + 2 < size) n |= static_cast<std::uint32_t>(data[i + 2]);
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out.push_back(i + 1 < size ? kB64[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < size ? kB64[n & 63] : '=');
  }
  return out;
}

std::string Base64Decode(std::string_view text) {
  std::array<std::int8_t, 256> table;
  table.fill(-1);
  for (std::size_t i = 0; i < kB64.size(); ++i) table[static_cast<unsigned char>(kB64[i])] = static_cast<std::int8_t>(i);
  std::string out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (const char c : text) {
    if (c == '=') break;
    const std::int8_t v = table[static_cast<unsigned char>(c)];
    if (v < 0) continue;
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

JsonValue CodecParToJson(const AVCodecParameters& par) {
  JsonObject o;
  o.emplace("codec_type", JsonValue(static_cast<std::int64_t>(par.codec_type)));
  o.emplace("codec_id", JsonValue(static_cast<std::int64_t>(par.codec_id)));
  o.emplace("codec_tag", JsonValue(static_cast<std::int64_t>(par.codec_tag)));
  o.emplace("format", JsonValue(static_cast<std::int64_t>(par.format)));
  o.emplace("bit_rate", JsonValue(par.bit_rate));
  o.emplace("profile", JsonValue(static_cast<std::int64_t>(par.profile)));
  o.emplace("level", JsonValue(static_cast<std::int64_t>(par.level)));
  o.emplace("width", JsonValue(static_cast<std::int64_t>(par.width)));
  o.emplace("height", JsonValue(static_cast<std::int64_t>(par.height)));
  o.emplace("sar", RationalJson(FromAv(par.sample_aspect_ratio)));
  o.emplace("framerate", RationalJson(FromAv(par.framerate)));
  o.emplace("color_range", JsonValue(static_cast<std::int64_t>(par.color_range)));
  o.emplace("color_primaries", JsonValue(static_cast<std::int64_t>(par.color_primaries)));
  o.emplace("color_trc", JsonValue(static_cast<std::int64_t>(par.color_trc)));
  o.emplace("color_space", JsonValue(static_cast<std::int64_t>(par.color_space)));
  o.emplace("sample_rate", JsonValue(static_cast<std::int64_t>(par.sample_rate)));
  o.emplace("frame_size", JsonValue(static_cast<std::int64_t>(par.frame_size)));
  o.emplace("channels", JsonValue(static_cast<std::int64_t>(par.ch_layout.nb_channels)));
  char layout[64] = {};
  av_channel_layout_describe(&par.ch_layout, layout, sizeof(layout));
  o.emplace("channel_layout", JsonValue(layout));
  if (par.extradata != nullptr && par.extradata_size > 0) {
    o.emplace("extradata", JsonValue(Base64Encode(par.extradata, static_cast<std::size_t>(par.extradata_size))));
  }
  return JsonValue(std::move(o));
}

Status CodecParFromJson(const JsonValue& v, AVCodecParameters* out) {
  if (!v.is_object()) return Status::InvalidArgument("codecpar must be an object");
  out->codec_type = static_cast<AVMediaType>(v.GetInteger("codec_type").value_or(AVMEDIA_TYPE_UNKNOWN));
  out->codec_id = static_cast<AVCodecID>(v.GetInteger("codec_id").value_or(AV_CODEC_ID_NONE));
  out->codec_tag = static_cast<std::uint32_t>(v.GetInteger("codec_tag").value_or(0));
  out->format = static_cast<int>(v.GetInteger("format").value_or(-1));
  out->bit_rate = v.GetInteger("bit_rate").value_or(0);
  out->profile = static_cast<int>(v.GetInteger("profile").value_or(AV_PROFILE_UNKNOWN));
  out->level = static_cast<int>(v.GetInteger("level").value_or(AV_LEVEL_UNKNOWN));
  out->width = I32(v, "width");
  out->height = I32(v, "height");
  if (const auto r = RationalFrom(v.Find("sar"))) out->sample_aspect_ratio = ToAv(*r);
  if (const auto r = RationalFrom(v.Find("framerate"))) out->framerate = ToAv(*r);
  out->color_range = static_cast<AVColorRange>(v.GetInteger("color_range").value_or(AVCOL_RANGE_UNSPECIFIED));
  out->color_primaries = static_cast<AVColorPrimaries>(v.GetInteger("color_primaries").value_or(AVCOL_PRI_UNSPECIFIED));
  out->color_trc = static_cast<AVColorTransferCharacteristic>(v.GetInteger("color_trc").value_or(AVCOL_TRC_UNSPECIFIED));
  out->color_space = static_cast<AVColorSpace>(v.GetInteger("color_space").value_or(AVCOL_SPC_UNSPECIFIED));
  out->sample_rate = I32(v, "sample_rate");
  out->frame_size = I32(v, "frame_size");
  const std::string layout = v.GetString("channel_layout").value_or("");
  av_channel_layout_uninit(&out->ch_layout);
  if (!layout.empty() && av_channel_layout_from_string(&out->ch_layout, layout.c_str()) < 0) {
    av_channel_layout_default(&out->ch_layout, I32(v, "channels"));
  } else if (layout.empty() && I32(v, "channels") > 0) {
    av_channel_layout_default(&out->ch_layout, I32(v, "channels"));
  }
  if (out->extradata != nullptr) {
    av_freep(&out->extradata);
    out->extradata_size = 0;
  }
  if (const auto ed = v.GetString("extradata")) {
    const std::string bytes = Base64Decode(*ed);
    out->extradata = static_cast<std::uint8_t*>(av_mallocz(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (out->extradata == nullptr) return Status::ResourceExhausted("extradata alloc failed");
    std::memcpy(out->extradata, bytes.data(), bytes.size());
    out->extradata_size = static_cast<int>(bytes.size());
  }
  return Status::Ok();
}

}  // namespace ge::media
