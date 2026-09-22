#ifndef GE_MEDIA_MIX_H_
#define GE_MEDIA_MIX_H_

// N-way video composition and audio mixing. MixTemplate builds the graph and
// mutation patches; MixController applies them to a session.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ge/cpp/graph_spec.h>
#include <ge/cpp/json.h>
#include <ge/cpp/session.h>
#include <ge/cpp/types.h>

namespace ge::media {

struct MixRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  friend bool operator==(const MixRect&, const MixRect&) = default;
};

// cover: fill the rect, cropping the overflow (default); contain: fit inside
enum class MixFit : std::uint8_t { kCover, kContain };

// One video window. |rect| empty (w == 0) means "auto place in the grid".
struct SlotSpec {
  std::string member_id;
  // Compose/amix input port feeding this member ("main", "s1".."s16"),
  // assigned by MixTemplate::Graph / MixController from the member order.
  std::string port;
  MixRect rect;
  MixFit fit = MixFit::kCover;
  int z = 0;          // higher paints later (on top)
  bool visible = true;
  std::int64_t gain_millis = 1000;  // audio gain for this member, 0 = muted
  bool has_gain = false;            // gain_millis is meaningful
  friend bool operator==(const SlotSpec&, const SlotSpec&) = default;
};

struct LayoutSpec {
  int width = 1280;
  int height = 720;
  std::string pixel_format;  // empty: the negotiated output format
  std::int64_t gap = 0;      // grid spacing in pixels
  std::int64_t fps = 30;     // output frame rate
  // Rect drawn (or painted) for a member with no frame yet and for a frozen
  // member past the escalation threshold.
  std::string background = "black";
  // Per-slot overrides. Members without an entry are gridded automatically.
  std::vector<SlotSpec> slots;
  // A slot keeps the grid when it has no explicit rect; a member with an
  std::string grid = "auto";  // "auto" | "none"
};

struct MixOptions {
  std::int64_t window_ms = 40;        // MX-S-2: alignment window
  std::string on_missing = "freeze";  // freeze | black | placeholder | skip
  std::int64_t freeze_upgrade_ms = 10000;
  std::string reference = "main";    // "main" | "auto" (earliest of the rest)
  bool drop_late = true;             // MX-S-4: late frames are dropped, not phase-shifted
  std::int64_t queue_ms = 500;       // per-member backlog kept for reordering
  bool audio = true;
  std::int64_t queue_capacity = 32;
};

struct MixSpec {
  std::string member_id;          // stable id fragment
  std::string video_path;         // empty: audio-only member
  std::string audio_path;         // empty: reuse video_path (only if no stream grouping)
  bool has_audio_stream = true;   // the video input path also carries audio
  MixRect rect;
  bool has_rect = false;
  MixFit fit = MixFit::kCover;
  int z = 0;
  std::int64_t start_ms = 0;
  std::int64_t end_ms = 0;
  std::int64_t gain_millis = 1000;
  bool has_gain = false;
  friend bool operator==(const MixSpec&, const MixSpec&) = default;
};

struct MixNodeIds {
  std::string compose = "compose";
  std::string amix = "amix";
  std::string venc = "venc";
  std::string mux = "mux";
  std::string MemberSource(std::string_view member_id) const;       // demux "m.<id>.src"
  std::string MemberAudioSource(std::string_view member_id) const;  // demux "m.<id>.asrc"
  std::string MemberDecode(std::string_view member_id) const;
  std::string MemberAudioDecode(std::string_view member_id) const;
  std::string MemberEntryEdge(std::string_view member_id) const;
  std::string MemberAudioEntryEdge(std::string_view member_id) const;
  std::string MemberComposeEdge(std::string_view member_id, std::string_view port) const;
};

struct MixResultSpec {
  std::string output_path;
  std::string container = "flv";
  std::string codec = "h264";
  std::int64_t bitrate_kbps = 2000;
  int gop = 60;
  std::string preset = "veryfast";
  std::int64_t audio_bitrate_kbps = 128;
  std::int64_t audio_sample_rate = 48000;
  int audio_channels = 2;
};

class MixTemplate final {
 public:
  static constexpr const char* kMainPort = "main";
  static constexpr int kMaxMembers = 16;

  [[nodiscard]] static std::string MemberPort(int index);  // 0 -> "main", else "s<i>"
  [[nodiscard]] static Status ValidateMember(const MixSpec& spec);
  [[nodiscard]] static const char* MemberSyncPort();

  [[nodiscard]] static Result<GraphSpec> Graph(const LayoutSpec& layout, const MixOptions& options,
                                               const std::vector<MixSpec>& members, const MixResultSpec& result,
                                               std::string_view graph_id = "mix");
  [[nodiscard]] static Result<MutationPatch> AddMember(const MixSpec& spec, int port_index, const LayoutSpec& layout,
                                                       const MixOptions& options);
  [[nodiscard]] static Result<MutationPatch> RemoveMember(const MixSpec& spec, int port_index, const MixOptions& options,
                                                          RemovePolicy policy, bool drain = true);

  // Compose node options (structural fields included; session creation).
  [[nodiscard]] static JsonValue ComposeParameters(const LayoutSpec& layout, const MixOptions& options);
  // Hot-updatable subset only (slots/gap/background/window/compensation):
  // what SetParameters accepts while the session runs.
  [[nodiscard]] static JsonValue ComposeHotParameters(const LayoutSpec& layout, const MixOptions& options);
  [[nodiscard]] static JsonValue MixParameters(const MixOptions& options, const std::vector<SlotSpec>& slots);
  // Layout/mix parameters derived from a member list.
  [[nodiscard]] static std::vector<SlotSpec> Slots(const std::vector<MixSpec>& members, const LayoutSpec& layout);
  // Slots resolved with their ports assigned by member order -- what the
  // compose/amix nodes are configured with.
  [[nodiscard]] static LayoutSpec EffectiveLayout(const LayoutSpec& layout, const std::vector<MixSpec>& members);

  [[nodiscard]] static NodeSpec MemberDecodeNode(const MixSpec& spec);
  [[nodiscard]] static NodeSpec MemberAudioDecodeNode(const MixSpec& spec);
  [[nodiscard]] static NodeSpec ComposeNode(const LayoutSpec& layout, const MixOptions& options);
  [[nodiscard]] static NodeSpec MixNode(const LayoutSpec& layout, const MixOptions& options);
  [[nodiscard]] static NodeSpec EncodeNode(const MixResultSpec& result);
  [[nodiscard]] static NodeSpec AudioEncodeNode(const MixResultSpec& result);
  [[nodiscard]] static NodeSpec MuxNode(const MixResultSpec& result);
};

// Automatic grid geometry for |count| members on a |width|x|height| canvas
[[nodiscard]] MixRect GridCell(int index, int count, int width, int height, std::int64_t gap);

// Resolves the effective rect / z / fit of every member: explicit rects win,
// the rest are gridded on the canvas excluding the explicit ones.
[[nodiscard]] std::vector<SlotSpec> ResolveLayout(const std::vector<SlotSpec>& slots, int width, int height,
                                                  std::int64_t gap);

}  // namespace ge::media

#endif
