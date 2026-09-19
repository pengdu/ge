#ifndef GE_MEDIA_TRANSCODE_CONTROLLER_H_
#define GE_MEDIA_TRANSCODE_CONTROLLER_H_

// P6 task 3: host-side controller for one dynamic transcode session (03).
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
  // Codec/container switch (03 TR-U-3): the old branch is drain-removed and
  // |replacement| (a different id, usually a new output path) is added in the
  // same mutation, so the shared decoder never stops and the new encoder's
  // first packet is an IDR.
  [[nodiscard]] Result<OperationId> SwitchCodec(std::string_view id, const RenditionSpec& replacement);

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
  std::map<OperationId, std::pair<std::string, bool>> pending_removals_;  // op -> (id, drain)
  SubscriptionId subscription_ = 0;
  std::atomic<std::uint64_t> drain_timeouts_{0};
  std::atomic<std::uint64_t> format_changes_{0};
  std::atomic<std::uint64_t> node_failures_{0};
};

}  // namespace ge::media

#endif
