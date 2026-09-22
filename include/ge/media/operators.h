#ifndef GE_MEDIA_OPERATORS_H_
#define GE_MEDIA_OPERATORS_H_
// BuiltinOperatorFactory and hand that to EngineConfig::builtin_operators.
//
//   MediaDemux@1.0.0    source: input_path -> video(EncodedVideo), audio(EncodedAudio)
//   VideoDecode@1.0.0   EncodedVideo -> VideoFrame
//   AudioDecode@1.0.0   EncodedAudio -> AudioFrame
//   VideoScale@1.0.0    VideoFrame -> VideoFrame  (width/height, hot: watermark)
//   VideoConvert@1.0.0  VideoFrame -> VideoFrame  (pixel format per contract)
//   VideoFilter@1.0.0   VideoFrame -> VideoFrame  (any libavfilter chain, e.g. drawtext; hot: filter)
//   VideoEncode@1.0.0   VideoFrame -> EncodedVideo (codec/bitrate/gop; hot: bitrate_kbps, gop, force_idr)
//   AudioEncode@1.0.0   AudioFrame -> EncodedAudio (aac; bitrate_kbps)
//   MediaMux@1.0.0      sink: video(+audio) -> output_path (flv|mp4)
//   VideoCompose@1.0.0  N x VideoFrame -> VideoFrame (grid/PiP layout, alignment, compensation)
//   AudioMix@1.0.0      N x AudioFrame -> AudioFrame (per-member gain/mute)
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
inline constexpr std::string_view kOpVideoFilter = "VideoFilter@1.0.0";
inline constexpr std::string_view kOpVideoEncode = "VideoEncode@1.0.0";
inline constexpr std::string_view kOpAudioEncode = "AudioEncode@1.0.0";
inline constexpr std::string_view kOpMediaMux = "MediaMux@1.0.0";
inline constexpr std::string_view kOpVideoCompose = "VideoCompose@1.0.0";
inline constexpr std::string_view kOpAudioMix = "AudioMix@1.0.0";

// Event type published by VideoEncode ahead of every keyframe that starts
inline constexpr std::string_view kEventMediaFormatChanged = "media_format_changed";

// Event type published by a segmenting MediaMux (output_pattern) each time
// it rotates to a new file: {index, path, start_pts_ns}.
inline constexpr std::string_view kEventMediaSegment = "media_segment";

// Capability descriptors (also used by tests to assert negotiation).
[[nodiscard]] CapabilityDescriptor MediaDemuxCapability();
[[nodiscard]] CapabilityDescriptor VideoDecodeCapability();
[[nodiscard]] CapabilityDescriptor AudioDecodeCapability();
[[nodiscard]] CapabilityDescriptor VideoScaleCapability();
[[nodiscard]] CapabilityDescriptor VideoConvertCapability();
[[nodiscard]] CapabilityDescriptor VideoFilterCapability();
[[nodiscard]] CapabilityDescriptor VideoEncodeCapability();
[[nodiscard]] CapabilityDescriptor AudioEncodeCapability();
[[nodiscard]] CapabilityDescriptor MediaMuxCapability();
[[nodiscard]] CapabilityDescriptor VideoComposeCapability();
[[nodiscard]] CapabilityDescriptor AudioMixCapability();

// Factories.
[[nodiscard]] std::unique_ptr<Operator> MakeMediaDemux(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeVideoDecode(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeAudioDecode(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeVideoScale(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeVideoConvert(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeVideoFilter(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeVideoEncode(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeAudioEncode(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeMediaMux(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeVideoCompose(const OperatorCreateArgs& args);
[[nodiscard]] std::unique_ptr<Operator> MakeAudioMix(const OperatorCreateArgs& args);

// Registers every operator above.
void RegisterMediaOperators(BuiltinOperatorFactory& factory);
[[nodiscard]] std::shared_ptr<BuiltinOperatorFactory> MakeMediaOperatorFactory();

// Parses a libavfilter chain (the VideoFilter "filter" option) against a
// throwaway 64x64 yuv420p source; lets a host reject a bad chain before it
// issues the mutation (the operator's Open runs the same check).
[[nodiscard]] Status ValidateFilterChain(std::string_view chain);

// Encoder availability probe (which H.264 encoder Open would pick, or
// "mpeg4" fallback). Lets tests avoid hard-coding codec names.
[[nodiscard]] std::string PreferredVideoEncoder(std::string_view codec = "h264");

// FFmpeg's global log level (process-wide). Long runs (ge_soak) call
// SetFfmpegLogLevel(FfmpegLogLevel::kError) so encoder statistics do not
// flood the host log; default is FFmpeg's (info).
enum class FfmpegLogLevel { kQuiet, kError, kWarning, kInfo };
void SetFfmpegLogLevel(FfmpegLogLevel level) noexcept;

}  // namespace ge::media

#endif
