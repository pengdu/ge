// 12 §12.3 audit log (AUD-1 / AUD-2, TD-13).
#include <ge/cpp/audit_log.h>

#include <algorithm>
#include <cctype>

#include "clock.h"

namespace ge {

// ---------------------------------------------------------------------------
// Redaction (AUD-2)
// ---------------------------------------------------------------------------

bool IsSensitiveKey(std::string_view key) noexcept {
  std::string lower;
  lower.reserve(key.size());
  for (const char c : key) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  static constexpr std::string_view kNeedles[] = {"key",      "token",         "secret", "password",
                                                  "passwd",   "credential",    "auth",   "cookie",
                                                  "signature"};
  for (const std::string_view n : kNeedles) {
    if (lower.find(n) != std::string::npos) return true;
  }
  return false;
}

std::string RedactUrl(std::string_view url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return std::string(url);
  std::string out(url.substr(0, scheme_end + 3));
  std::string_view rest = url.substr(scheme_end + 3);
  // Drop query and fragment.
  if (const auto q = rest.find_first_of("?#"); q != std::string_view::npos) rest = rest.substr(0, q);
  // userinfo@host -> ***@host
  const auto slash = rest.find('/');
  const std::string_view authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
  if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
    out += "***@";
    out += authority.substr(at + 1);
  } else {
    out += authority;
  }
  if (slash != std::string_view::npos) out += rest.substr(slash);
  return out;
}

namespace {

bool LooksLikeUrl(std::string_view s) noexcept {
  const auto p = s.find("://");
  if (p == std::string_view::npos || p == 0 || p > 16) return false;
  for (const char c : s.substr(0, p)) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') return false;
  }
  return true;
}

JsonValue RedactImpl(const JsonValue& v, std::size_t max_string, std::size_t max_array, int depth) {
  if (depth > 32) return JsonValue("...");
  if (v.is_string()) {
    const std::string& s = v.as_string();
    if (LooksLikeUrl(s)) return JsonValue(RedactUrl(s));
    if (s.size() > max_string) {
      return JsonValue("<" + std::to_string(s.size()) + " chars: " + s.substr(0, max_string / 4) + "...>");
    }
    return v;
  }
  if (v.is_array()) {
    const JsonArray& in = v.as_array();
    JsonArray out;
    const std::size_t n = std::min(in.size(), max_array);
    out.reserve(n + 1);
    for (std::size_t i = 0; i < n; ++i) out.push_back(RedactImpl(in[i], max_string, max_array, depth + 1));
    if (in.size() > max_array) out.push_back(JsonValue("<+" + std::to_string(in.size() - max_array) + " more>"));
    return JsonValue(std::move(out));
  }
  if (v.is_object()) {
    JsonObject out;
    for (const auto& [k, val] : v.as_object()) {
      if (IsSensitiveKey(k)) {
        out.emplace(k, JsonValue("***"));
      } else {
        out.emplace(k, RedactImpl(val, max_string, max_array, depth + 1));
      }
    }
    return JsonValue(std::move(out));
  }
  return v;
}

}  // namespace

JsonValue Redact(const JsonValue& v, std::size_t max_string, std::size_t max_array) {
  return RedactImpl(v, max_string, max_array, 0);
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

JsonValue AuditRecord::ToJson() const {
  JsonObject o;
  o["audit_id"] = JsonValue(audit_id);
  o["timestamp_ns"] = JsonValue(timestamp_ns);
  if (!caller.caller_id.empty()) o["caller_id"] = JsonValue(caller.caller_id);
  if (!caller.request_id.empty()) o["request_id"] = JsonValue(caller.request_id);
  o["operation"] = JsonValue(operation);
  o["target"] = JsonValue(target);
  if (session_id != 0) o["session_id"] = JsonValue(session_id);
  if (operation_id) o["operation_id"] = JsonValue(*operation_id);
  if (topology_version) o["topology_version"] = JsonValue(*topology_version);
  if (parameter_version) o["parameter_version"] = JsonValue(*parameter_version);
  o["result"] = JsonValue(Status::CodeName(result));
  if (!message.empty()) o["message"] = JsonValue(message);
  if (digest.is_object() && !digest.as_object().empty()) o["digest"] = digest;
  return JsonValue(std::move(o));
}

Result<AuditFilter> AuditFilter::FromJson(const JsonValue& v) {
  AuditFilter f;
  if (v.is_null()) return f;
  if (!v.is_object()) return Status::InvalidArgument("audit filter must be an object");
  if (const auto s = v.GetInteger("session_id")) {
    if (*s < 0) return Status::InvalidArgument("session_id must be >= 0");
    f.session_id = static_cast<SessionId>(*s);
  }
  if (const auto s = v.GetString("operation")) f.operation = *s;
  if (const auto s = v.GetString("caller_id")) f.caller_id = *s;
  if (const auto s = v.GetString("request_id")) f.request_id = *s;
  if (const auto b = v.GetBool("failures_only")) f.failures_only = *b;
  if (const auto n = v.GetInteger("since_ns")) f.since_ns = *n;
  if (const auto n = v.GetInteger("after_audit_id")) {
    if (*n < 0) return Status::InvalidArgument("after_audit_id must be >= 0");
    f.after_audit_id = static_cast<std::uint64_t>(*n);
  }
  if (const auto n = v.GetInteger("limit")) {
    if (*n < 0) return Status::InvalidArgument("limit must be >= 0");
    f.limit = static_cast<std::size_t>(*n);
  }
  return f;
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

AuditLog::AuditLog(std::size_t capacity) : capacity_(std::max<std::size_t>(1, capacity)) {}

void AuditLog::SetSink(Sink sink) {
  std::lock_guard lock(mutex_);
  sink_ = std::move(sink);
}

void AuditLog::Append(AuditRecord record) {
  record.digest = Redact(record.digest);
  if (record.timestamp_ns == 0) record.timestamp_ns = WallClockNs();
  Sink sink;
  {
    std::lock_guard lock(mutex_);
    record.audit_id = next_id_++;
    ring_.push_back(record);
    while (ring_.size() > capacity_) {
      ring_.pop_front();
      ++dropped_;
    }
    sink = sink_;
  }
  if (sink) {
    try {
      sink(record);
    } catch (...) {
      // An audit consumer must not fail the engine (13 §7.4).
    }
  }
}

std::vector<AuditRecord> AuditLog::Query(const AuditFilter& filter) const {
  std::lock_guard lock(mutex_);
  std::vector<AuditRecord> out;
  for (const AuditRecord& r : ring_) {
    if (r.audit_id <= filter.after_audit_id) continue;
    if (r.timestamp_ns < filter.since_ns) continue;
    if (filter.session_id && r.session_id != *filter.session_id) continue;
    if (filter.operation && r.operation != *filter.operation) continue;
    if (filter.caller_id && r.caller.caller_id != *filter.caller_id) continue;
    if (filter.request_id && r.caller.request_id != *filter.request_id) continue;
    if (filter.failures_only && r.result == GE_STATUS_OK) continue;
    out.push_back(r);
    if (filter.limit != 0 && out.size() >= filter.limit) break;
  }
  return out;
}

JsonValue AuditLog::QueryJson(const AuditFilter& filter) const {
  JsonArray arr;
  for (const AuditRecord& r : Query(filter)) arr.push_back(r.ToJson());
  JsonObject o;
  o["records"] = JsonValue(std::move(arr));
  o["last_audit_id"] = JsonValue(last_audit_id());
  o["dropped"] = JsonValue(dropped());
  return JsonValue(std::move(o));
}

std::size_t AuditLog::size() const {
  std::lock_guard lock(mutex_);
  return ring_.size();
}

std::uint64_t AuditLog::last_audit_id() const {
  std::lock_guard lock(mutex_);
  return next_id_ - 1;
}

std::uint64_t AuditLog::dropped() const {
  std::lock_guard lock(mutex_);
  return dropped_;
}

}  // namespace ge
