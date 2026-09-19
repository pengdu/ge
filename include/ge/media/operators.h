#ifndef GE_MEDIA_OPERATORS_H_
#define GE_MEDIA_OPERATORS_H_

// P6 builtin media operators (ge_media). Register them on a
// BuiltinOperatorFactory and hand that to EngineConfig::builtin_operators.
//
//   MediaDemux@1.0.0    source: input_path -> video(EncodedVideo), audio(EncodedAudio)
//   VideoDecode@1.0.0   EncodedVideo -> VideoFrame
//   AudioDecode@1.0.0   EncodedAudio -> AudioFrame
//   VideoScale@1.0.0    VideoFrame -> VideoFrame  (width/height, hot: watermark)
//   VideoConvert@1.0.0  VideoFrame -> VideoFrame  (pixel format per contract)
//   VideoEncode@1.0.0   VideoFrame -> EncodedVideo (codec/bitrate/gop; hot: bitrate_kbps, gop, force_idr)
//   AudioEncode@1.0.0   AudioFrame -> EncodedAudio (aac; bitrate_kbps)
//   MediaMux@1.0.0      sink: video(+audio) -> output_path (flv|mp4)
//
// All operators are stateful, parallelism 1, software backends; other
// CodecBackend values are accepted by the option parser and rejected at
// Open with RESOURCE_EXHAUSTED until a hardware build provides them.

#include <memory>
#include <string_view>

#include <ge/cpp/capability.h>
#include <ge/cpp/operator.h>

namespace ge::media {

inline constexpr std::string_view kOpMediaDemux = "MediaDemux@1.0.0";
inline constexpr std::string_view kOpVideoDecode = "VideoDecode@1.0.0";
inline constexpr std::string_view kOpAudioDecode = "AudioDecode@1.0.0";
inline constexpr std::string_view kOpVideoScale = "VideoScale@1.0.0";
inline constexpr std::string_view kOpVideoConvert = "VideoConvert@1.0.0";
inline constexpr std::string_view kOpVideoEncode = "VideoEncode@1.0.0";
inline constexpr std::string_view kOpAudioEncode = "AudioEncode@1.0.0";
inline constexpr std::string_view kOpMediaMux = "MediaMux@1.0.0";

// Event type published by VideoEncode ahead of every keyframe that starts
// a new stream configuration (12 §4.5 / §12.2).
inline constexpr std::string_view kEventMediaFormatChanged = "media_format_changed";

// Capability descriptors (also used by tests to assert negotiation).
[[nodiscard]] CapabilityDescriptor MediaDemuxCapability();
[[nodiscard]] CapabilityDescriptor VideoDecodeCapability();
[[nodiscard]] CapabilityDescriptor AudioDecodeCapability();
[[nodiscard]] CapabilityDescriptor VideoScaleCapability();
[[nodiscard]] CapabilityDescriptor VideoConvertCapability();
[[nodiscard]] CapabilityDescriptor VideoEncodeCapability();
[[nodiscard]] CapabilityDescriptor AudioEncodeCapability();
[[nodiscard]] CapabilityDescriptor MediaMuxCapability();

// Factories.
[[nodiscard]] std::unique_ptr<Operator> MakeMediaDemux(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeVideoDecode(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeAudioDecode(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeVideoScale(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeVideoConvert(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeVideoEncode(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeAudioEncode(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeMediaMux(const OperatorCreateArgs& args);

// Registers every operator above.
void RegisterMediaOperators(BuiltinOperatorFactory& factory);
[[nodiscard]] std::shared_ptr<BuiltinOperatorFactory> MakeMediaOperatorFactory();

// Encoder availability probe (which H.264 encoder Open would pick, or
// "mpeg4" fallback). Lets tests avoid hard-coding codec names.
[[nodiscard]] std::string PreferredVideoEncoder(std::string_view codec = "h264");

}  // namespace ge::media

#endif
