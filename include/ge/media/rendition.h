#ifndef GE_MEDIA_RENDITION_H_
#define GE_MEDIA_RENDITION_H_

// P6 task 3: rendition subgraph template (03 §4 "档位级": Scale -> Encode
// -> Mux branch) and the mutation patches that add/remove/update one.
//
// Base graph (shared, never touched by rendition changes):
//   demux.video -> vdec.in            demux.audio -> adec.in -> aenc.in
// Rendition <id> (three nodes, four edges, two entry edges):
//   vdec.out -> r.<id>.scale -> r.<id>.venc -> r.<id>.mux.video
//   aenc.out ------------------------------> r.<id>.mux.audio

#include <optional>
#include <string>
#include <vector>

#include <ge/cpp/graph_spec.h>
#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge::media {

struct WatermarkSpec {
  int x = 0, y = 0, w = 0, h = 0;
  std::string color = "white";
  [[nodiscard]] JsonValue ToJson() const;
  friend bool operator==(const WatermarkSpec&, const WatermarkSpec&) = default;
};

struct RenditionSpec {
  std::string id;  // stable id fragment: [A-Za-z][A-Za-z0-9_.-]*
  int width = 1280;
  int height = 720;
  std::int64_t bitrate_kbps = 2000;
  std::string codec = "h264";
  int gop = 60;
  int fps = 0;  // 0: encoder default (30)
  std::optional<WatermarkSpec> watermark;
  std::string output_path;
  std::string container = "flv";
  std::string preset = "veryfast";
  bool audio = true;  // connect the shared AAC stream
  std::uint32_t queue_capacity = 64;
  friend bool operator==(const RenditionSpec&, const RenditionSpec&) = default;
};

struct RenditionNodeIds {
  std::string scale, venc, mux;
  std::string video_entry_edge, audio_entry_edge;
  std::string scale_venc_edge, venc_mux_edge;
};

struct TranscodeBaseOptions {
  std::string input_path;
  bool realtime = false;
  bool loop = false;
  bool audio = true;
  std::int64_t audio_bitrate_kbps = 128;
  std::string video_source_port = "vdec.out";
  std::string audio_source_port = "aenc.out";
  std::uint32_t queue_capacity = 64;
};

class RenditionTemplate final {
 public:
  using BaseOptions = TranscodeBaseOptions;

  // Node ids used by the base graph.
  static constexpr const char* kDemux = "demux";
  static constexpr const char* kVideoDecode = "vdec";
  static constexpr const char* kAudioDecode = "adec";
  static constexpr const char* kAudioEncode = "aenc";

  [[nodiscard]] static RenditionNodeIds Ids(std::string_view rendition_id);
  [[nodiscard]] static Status ValidateSpec(const RenditionSpec& spec);

  // Shared upstream graph (demux/decoders/audio encoder), no renditions.
  [[nodiscard]] static Result<GraphSpec> BaseGraph(const BaseOptions& options, std::string_view graph_id = "transcode");
  // Same as BaseGraph plus the given renditions (for CreateSession).
  [[nodiscard]] static Result<GraphSpec> Graph(const BaseOptions& options, const std::vector<RenditionSpec>& renditions,
                                               std::string_view graph_id = "transcode");

  // Patches. Video/audio source ports default to the base graph's.
  [[nodiscard]] static Result<MutationPatch> AddRendition(const RenditionSpec& spec, const BaseOptions& base = {});
  [[nodiscard]] static MutationPatch RemoveRendition(std::string_view rendition_id, RemovePolicy policy, bool audio = true,
                                                     const BaseOptions& base = {});
  // Hot-updatable subset: bitrate/gop/force_idr -> venc, watermark -> scale.
  struct HotUpdate {
    std::optional<std::int64_t> bitrate_kbps;
    std::optional<int> gop;
    std::optional<WatermarkSpec> watermark;
    bool clear_watermark = false;
    bool force_idr = false;
  };
  [[nodiscard]] static JsonValue EncoderParameters(const HotUpdate& u);
  [[nodiscard]] static JsonValue ScaleParameters(const HotUpdate& u);

  [[nodiscard]] static NodeSpec ScaleNode(const RenditionSpec& spec);
  [[nodiscard]] static NodeSpec EncodeNode(const RenditionSpec& spec);
  [[nodiscard]] static NodeSpec MuxNode(const RenditionSpec& spec);
};

}  // namespace ge::media

#endif
