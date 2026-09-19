#include <ge/infer/tensor.h>

#include <ge/cpp/json.h>

namespace ge::infer {

std::string_view ToString(DType d) noexcept {
  switch (d) {
    case DType::kFloat32: return "float32";
    case DType::kUint8: return "uint8";
    case DType::kInt64: return "int64";
  }
  return "?";
}

std::optional<DType> ParseDType(std::string_view s) noexcept {
  if (s == "float32") return DType::kFloat32;
  if (s == "uint8") return DType::kUint8;
  if (s == "int64") return DType::kInt64;
  return std::nullopt;
}

std::size_t SizeOf(DType d) noexcept {
  switch (d) {
    case DType::kFloat32: return 4;
    case DType::kUint8: return 1;
    case DType::kInt64: return 8;
  }
  return 0;
}

std::size_t TensorFormat::ElementCount() const noexcept {
  std::size_t n = 1;
  for (const std::int64_t d : shape) {
    if (d <= 0) return 0;
    n *= static_cast<std::size_t>(d);
  }
  return shape.empty() ? 0 : n;
}

std::string TensorFormat::ToJson() const {
  JsonArray dims;
  for (const std::int64_t d : shape) dims.push_back(JsonValue(d));
  return JsonValue(JsonObject{{"dtype", JsonValue(std::string(ToString(dtype)))}, {"shape", JsonValue(std::move(dims))}})
      .Serialize();
}

Result<TensorFormat> TensorFormat::FromJson(std::string_view json) {
  JsonParseResult parsed = ParseJson(json);
  if (!parsed.ok()) return Status::InvalidArgument("tensor format: " + parsed.error);
  const JsonValue& v = *parsed.value;
  if (!v.is_object()) return Status::InvalidArgument("tensor format must be an object");
  TensorFormat f;
  const auto dtype = v.GetString("dtype");
  if (!dtype) return Status::InvalidArgument("tensor format: missing dtype");
  const auto parsed_dtype = ParseDType(*dtype);
  if (!parsed_dtype) return Status::InvalidArgument("tensor format: unsupported dtype '" + *dtype + "'");
  f.dtype = *parsed_dtype;
  const JsonValue* shape = v.Find("shape");
  if (shape == nullptr || !shape->is_array()) return Status::InvalidArgument("tensor format: missing shape");
  for (const JsonValue& d : shape->as_array()) {
    if (!d.is_integer() || d.as_integer() <= 0) return Status::InvalidArgument("tensor format: shape dims must be positive integers");
    f.shape.push_back(d.as_integer());
  }
  if (f.shape.empty()) return Status::InvalidArgument("tensor format: shape must not be empty");
  return f;
}

Result<TensorFormat> FormatOf(const Packet& packet) {
  const std::string* json = packet.meta().Get(metadata_keys::kFormat);
  if (json == nullptr) return Status::InvalidArgument("tensor packet has no format metadata");
  auto f = TensorFormat::FromJson(*json);
  if (!f.ok()) return f.status();
  const std::size_t bytes = packet.payload ? packet.payload->size : 0;
  if (bytes != f->ByteSize()) {
    return Status::InvalidArgument("tensor payload is " + std::to_string(bytes) + " bytes, format needs " +
                                   std::to_string(f->ByteSize()));
  }
  return f;
}

Packet MakeTensorPacket(BufferRef payload, const TensorFormat& format, PacketSeq seq, std::int64_t pts_ns) {
  Packet p;
  p.header.seq = seq;
  p.header.pts_ns = pts_ns;
  p.header.type_tag = TypeTagRegistry::Global().Intern(kTagTensor);
  p.payload = std::move(payload);
  auto m = std::make_shared<Metadata>();
  m->Set(metadata_keys::kFormat, format.ToJson());
  p.metadata = std::move(m);
  return p;
}

}  // namespace ge::infer
