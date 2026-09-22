#ifndef GE_CPP_AUDIT_LOG_H_
#define GE_CPP_AUDIT_LOG_H_

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ge/cpp/json.h>
#include <ge/cpp/types.h>

namespace ge {

//
// One AuditLog per Engine. Every control operation (session.create /
// session.stop / mutation.apply / parameter.set / plugin.*) is appended once
// when it reaches a terminal state, plus resource rejections and node
// failures. Records are kept in a bounded ring (oldest dropped), pushed to
// an optional sink callback on the appending thread, and queryable as JSON.
// The `digest` is always passed through Redact() before it is stored.

struct AuditRecord {
  std::uint64_t audit_id = 0;
  std::int64_t timestamp_ns = 0;
  CallerContext caller;
  std::string operation;  // session.create / mutation.apply / plugin.load / node.failed / resource.rejected
  std::string target;     // session:<id> / node:<id> / plugin:<id> / engine
  SessionId session_id = 0;
  std::optional<OperationId> operation_id;
  std::optional<TopologyVersion> topology_version;
  std::optional<ParameterVersion> parameter_version;
  ge_status_code result = GE_STATUS_OK;
  std::string message;
  JsonValue digest = JsonValue(JsonObject{});  // redacted (AUD-2)
  [[nodiscard]] JsonValue ToJson() const;
};

struct AuditFilter {
  std::optional<SessionId> session_id;
  std::optional<std::string> operation;  // exact match
  std::optional<std::string> caller_id;
  std::optional<std::string> request_id;
  bool failures_only = false;  // result != OK
  std::int64_t since_ns = 0;
  std::uint64_t after_audit_id = 0;  // cursor: records with audit_id > this
  std::size_t limit = 0;             // 0 == no limit
  [[nodiscard]] static Result<AuditFilter> FromJson(const JsonValue& v);
};

// AUD-2 redaction of a JSON digest, recursively:
//   - keys containing key/token/secret/password/credential/authorization
//     (case-insensitive) -> "***";
//   - string values that look like URLs (scheme://) keep scheme, host and
//     path; query and fragment are dropped, userinfo replaced by "***";
//   - strings longer than |max_string| are cut to a size note;
//   - arrays longer than |max_array| are cut to their first entries plus a
//     count.
[[nodiscard]] JsonValue Redact(const JsonValue& v, std::size_t max_string = 256, std::size_t max_array = 32);
[[nodiscard]] std::string RedactUrl(std::string_view url);
[[nodiscard]] bool IsSensitiveKey(std::string_view key) noexcept;

class AuditLog final {
 public:
  using Sink = std::function<void(const AuditRecord&)>;

  explicit AuditLog(std::size_t capacity = 4096);

  // Redacts |record.digest|, stamps id/timestamp, appends, then calls the
  // sink (if any) on the calling thread. Never throws; a throwing sink is
  // swallowed so an audit consumer cannot fail an engine operation.
  void Append(AuditRecord record);
  void SetSink(Sink sink);

  [[nodiscard]] std::vector<AuditRecord> Query(const AuditFilter& filter) const;
  [[nodiscard]] JsonValue QueryJson(const AuditFilter& filter) const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::uint64_t last_audit_id() const;
  [[nodiscard]] std::uint64_t dropped() const;  // ring overflow count

 private:
  mutable std::mutex mutex_;
  std::deque<AuditRecord> ring_;
  std::size_t capacity_;
  std::uint64_t next_id_ = 1;
  std::uint64_t dropped_ = 0;
  Sink sink_;
};

}  // namespace ge

#endif
