#ifndef GE_MEDIA_TRANSCODE_CONTROLLER_H_
#define GE_MEDIA_TRANSCODE_CONTROLLER_H_
// Wraps RenditionTemplate patches into Session::Apply / SetParameters calls,
// tracks the live rendition set and listens to drain_timeout /
// media_format_changed events for the session.

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <ge/cpp/engine.h>
#include <ge/cpp/session.h>
#include <ge/media/rendition.h>

namespace ge::media {

struct TranscodeStats {
  std::uint64_t drain_timeouts = 0;
  std::uint64_t format_changes = 0;
  std::uint64_t node_failures = 0;
};

class TranscodeController final {
 public:
  // The session must have been created from RenditionTemplate::Graph(base, ...)
  // on |engine|; |initial| lists the renditions it already contains.
  TranscodeController(Engine& engine, Session& session, RenditionTemplate::BaseOptions base,
                      std::vector<RenditionSpec> initial = {});
  ~TranscodeController();
  TranscodeController(const TranscodeController&) = delete;
  TranscodeController& operator=(const TranscodeController&) = delete;

  [[nodiscard]] Result<OperationId> AddRendition(const RenditionSpec& spec);
  [[nodiscard]] Result<OperationId> RemoveRendition(std::string_view id, bool drain = true);
  // bitrate/gop/force_idr -> venc, watermark -> scale. Returns the last
  // parameter update issued.
  [[nodiscard]] Result<ParameterUpdate> UpdateRendition(std::string_view id, const RenditionTemplate::HotUpdate& u);
  [[nodiscard]] Result<ParameterUpdate> RequestKeyFrame(std::string_view id);
  // |replacement| (a different id, usually a new output path) is added in the
  // same mutation, so the shared decoder never stops and the new encoder's
  // first packet is an IDR.
  [[nodiscard]] Result<OperationId> SwitchCodec(std::string_view id, const RenditionSpec& replacement);

  // (or scale) and the encoder. Insert is an InsertChain, remove a bypass
  // RemoveChain (drain by default so no frame in the filter is lost), so
  // the rendition's encoder/mux keep running; the surrounding renditions
  // are untouched. Update hot-swaps the chain at the next frame boundary.
  // Insert splits the edge that feeds the encoder in the *published*
  // topology, so Wait() for a previous filter change on the same rendition
  // before issuing the next one.
  [[nodiscard]] Result<OperationId> InsertFilter(std::string_view id, const FilterSpec& filter);
  [[nodiscard]] Result<OperationId> RemoveFilter(std::string_view id, std::string_view filter_id, bool drain = true);
  [[nodiscard]] Result<ParameterUpdate> UpdateFilter(std::string_view id, std::string_view filter_id,
                                                     std::string_view chain);
  // Current filter order (scale side first) of a rendition; empty when the
  // rendition is unknown.
  [[nodiscard]] std::vector<FilterSpec> Filters(std::string_view id) const;

  // Waits for an operation, pumping the session when the engine runs its
  // executor inline (cpu_threads == 0).
  [[nodiscard]] Result<OperationRecord> Wait(OperationId op, std::chrono::milliseconds timeout);

  [[nodiscard]] std::vector<RenditionSpec> Renditions() const;
  [[nodiscard]] std::optional<RenditionSpec> Find(std::string_view id) const;
  [[nodiscard]] TranscodeStats stats() const;
  [[nodiscard]] Session& session() noexcept { return session_; }
  [[nodiscard]] const RenditionTemplate::BaseOptions& base() const noexcept { return base_; }

 private:
  [[nodiscard]] std::optional<RenditionSpec> Lookup(std::string_view id) const;
  void Pump();

  Engine& engine_;
  Session& session_;
  RenditionTemplate::BaseOptions base_;
  mutable std::mutex mutex_;
  std::map<std::string, RenditionSpec> renditions_;
  std::map<std::string, std::vector<FilterSpec>> filters_;  // rendition id -> filters, scale side first
  std::map<OperationId, std::pair<std::string, bool>> pending_removals_;  // op -> (id, drain)
  SubscriptionId subscription_ = 0;
  std::atomic<std::uint64_t> drain_timeouts_{0};
  std::atomic<std::uint64_t> format_changes_{0};
  std::atomic<std::uint64_t> node_failures_{0};
};

}  // namespace ge::media

#endif
