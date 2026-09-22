#ifndef GE_CPP_OPERATION_H_
#define GE_CPP_OPERATION_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge {

// CTL-6: accepted / running / succeeded / failed / cancelled.
enum class OperationState : std::uint8_t { kAccepted, kRunning, kSucceeded, kFailed, kCancelled };
[[nodiscard]] std::string_view ToString(OperationState s) noexcept;

struct OperationRecord {
  OperationId id = 0;
  std::string kind;  // session.create / session.stop / mutation.apply / parameter.set
  SessionId session_id = 0;
  OperationState state = OperationState::kAccepted;
  Status result;
  std::optional<TopologyVersion> topology_version;
  std::optional<ParameterVersion> parameter_version;
  CallerContext caller;
  JsonValue detail = JsonValue(JsonObject{});
  std::int64_t created_ns = 0;
  std::int64_t finished_ns = 0;

  [[nodiscard]] bool terminal() const noexcept {
    return state == OperationState::kSucceeded || state == OperationState::kFailed ||
           state == OperationState::kCancelled;
  }
  [[nodiscard]] JsonValue ToJson() const;
};

// block on a condition variable keyed by id.
class OperationRegistry final {
 public:
  explicit OperationRegistry(std::size_t retain_terminal = 1024);

  // AUD-1: invoked once per operation, on the thread that moves it to a
  // terminal state, outside the registry lock, with the final record.
  using TerminalHook = std::function<void(const OperationRecord&)>;
  void SetTerminalHook(TerminalHook hook);

  [[nodiscard]] OperationId Create(std::string kind, SessionId session, CallerContext caller,
                                   JsonValue detail = JsonValue(JsonObject{}));
  void SetRunning(OperationId id);
  // Attaches the published topology version while the operation is still
  void SetTopologyVersion(OperationId id, TopologyVersion version);
  void Succeed(OperationId id, std::optional<TopologyVersion> version = std::nullopt,
               std::optional<ParameterVersion> parameter_version = std::nullopt,
               JsonValue detail = JsonValue());
  void Fail(OperationId id, Status status, JsonValue detail = JsonValue());
  void Cancel(OperationId id, std::string reason);

  [[nodiscard]] std::optional<OperationRecord> Get(OperationId id) const;
  // Blocks until terminal or timeout; returns the record if terminal.
  [[nodiscard]] std::optional<OperationRecord> Wait(OperationId id,
                                                    std::chrono::milliseconds timeout) const;

 private:
  void Finish(OperationId id, OperationState state, Status result, JsonValue detail,
              std::optional<TopologyVersion> tv, std::optional<ParameterVersion> pv);

  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  TerminalHook terminal_hook_;  // guarded by mutex_ (copied out before use)
  std::map<OperationId, OperationRecord> records_;
  std::vector<OperationId> terminal_order_;
  std::size_t retain_terminal_;
  OperationId next_id_ = 1;
};

}  // namespace ge

#endif
