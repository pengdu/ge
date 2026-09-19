#ifndef GE_INFER_TENSOR_H_
#define GE_INFER_TENSOR_H_

// Tensor packet convention for ge_infer (12 §2.2 "Tensor" builtin tag).
//
// payload: dense, row-major, host memory (MemoryKind::kHost), no padding.
// metadata[kFormat] = JSON {"dtype": "float32"|"uint8"|"int64", "shape": [d0, d1, ...]}
//
// One packet is one sample; a batch dimension, if the model wants one, is
// added by the consuming operator (OnnxInfer prepends it). Producers that
// already carry a batch dimension of 1 are accepted (leading 1 is squeezed).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ge/cpp/packet.h>
#include <ge/cpp/types.h>

namespace ge::infer {

inline constexpr std::string_view kTagTensor = "Tensor";

enum class DType : std::uint8_t { kFloat32, kUint8, kInt64 };
[[nodiscard]] std::string_view ToString(DType d) noexcept;
[[nodiscard]] std::optional<DType> ParseDType(std::string_view s) noexcept;
[[nodiscard]] std::size_t SizeOf(DType d) noexcept;

struct TensorFormat {
  DType dtype = DType::kFloat32;
  std::vector<std::int64_t> shape;

  [[nodiscard]] std::size_t ElementCount() const noexcept;
  [[nodiscard]] std::size_t ByteSize() const noexcept { return ElementCount() * SizeOf(dtype); }
  [[nodiscard]] std::string ToJson() const;
  [[nodiscard]] static Result<TensorFormat> FromJson(std::string_view json);
  friend bool operator==(const TensorFormat&, const TensorFormat&) = default;
};

// Reads the format from packet metadata and checks it against the payload
// size. INVALID_ARGUMENT when the metadata is missing or inconsistent.
[[nodiscard]] Result<TensorFormat> FormatOf(const Packet& packet);

// Builds a Tensor packet around |payload| (already filled) with |format| in
// metadata. |seq|/|pts_ns| are copied from the caller (usually the source
// frame so results stay correlated with their input).
[[nodiscard]] Packet MakeTensorPacket(BufferRef payload, const TensorFormat& format, PacketSeq seq,
                                      std::int64_t pts_ns);

}  // namespace ge::infer

#endif
