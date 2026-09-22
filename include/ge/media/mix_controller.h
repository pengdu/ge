#ifndef GE_MEDIA_MIX_CONTROLLER_H_
#define GE_MEDIA_MIX_CONTROLLER_H_
// and the layout, issues the mutations / hot updates built by MixTemplate:
//
//   AddMember     new decode branch into a free compose/amix slot, then the
//   SetLayout     canvas-level layout replacement (slots/gap/background)
//
// Wait() pumps the inline engine like TranscodeController. Members() and

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ge/cpp/engine.h>
#include <ge/cpp/session.h>
#include <ge/media/mix.h>

namespace ge::media {

class MixController final {
 public:
  // The session must have been created from MixTemplate::Graph(layout,
  // options, initial, result) on |engine|.
  MixController(Engine& engine, Session& session, LayoutSpec layout, MixOptions options,
                std::vector<MixSpec> initial = {});
  ~MixController();
  MixController(const MixController&) = delete;
  MixController& operator=(const MixController&) = delete;

  [[nodiscard]] Result<OperationId> AddMember(const MixSpec& spec);
  [[nodiscard]] Result<OperationId> RemoveMember(std::string_view member_id, bool drain = true);

  // Layout / per-member window updates land at the next composed frame.
  [[nodiscard]] Result<ParameterUpdate> SetLayout(const LayoutSpec& layout);
  [[nodiscard]] Result<ParameterUpdate> MoveMember(std::string_view member_id, const MixRect& rect,
                                                   std::optional<MixFit> fit = std::nullopt,
                                                   std::optional<int> z = std::nullopt,
                                                   std::optional<bool> visible = std::nullopt);
  [[nodiscard]] Result<ParameterUpdate> SetGain(std::string_view member_id, std::int64_t gain_millis);

  [[nodiscard]] Result<OperationRecord> Wait(OperationId op, std::chrono::milliseconds timeout);

  [[nodiscard]] std::vector<MixSpec> Members() const;
  [[nodiscard]] LayoutSpec Layout() const;
  // Port ("main"/"s<i>") a member occupies; empty when unknown.
  [[nodiscard]] std::string PortOf(std::string_view member_id) const;

 private:
  [[nodiscard]] LayoutSpec ResolvedLocked() const;
  Status PushLayoutLocked();
  Result<ParameterUpdate> PushGainsLocked();
  [[nodiscard]] int FreePortLocked() const;
  void Pump();

  Engine& engine_;
  Session& session_;
  mutable std::mutex mutex_;
  LayoutSpec layout_;
  MixOptions options_;
  std::vector<MixSpec> members_;            // insertion order = paint fallback order
  std::map<std::string, int> ports_;        // member id -> slot index (0 = main)
};

}  // namespace ge::media

#endif
