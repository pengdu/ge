#include <ge/cpp/operation.h>

#include <algorithm>

#include "clock.h"

namespace ge {

std::string_view ToString(OperationState s) noexcept {
  switch (s) {
    case OperationState::kAccepted: return "accepted";
    case OperationState::kRunning: return "running";
    case OperationState::kSucceeded: return "succeeded";
    case OperationState::kFailed: return "failed";
    case OperationState::kCancelled: return "cancelled";
  }
  return "unknown";
}

JsonValue OperationRecord::ToJson() const {
  JsonObject o;
  o["operation_id"] = JsonValue(id);
  o["kind"] = JsonValue(kind);
  o["session_id"] = JsonValue(session_id);
  o["state"] = JsonValue(ToString(state));
  o["status"] = JsonValue(Status::CodeName(result.code()));
  if (!result.message().empty()) o["message"] = JsonValue(result.message());
  if (topology_version) o["topology_version"] = JsonValue(*topology_version);
  if (parameter_version) o["parameter_version"] = JsonValue(*parameter_version);
  if (!caller.caller_id.empty()) o["caller_id"] = JsonValue(caller.caller_id);
  if (!caller.request_id.empty()) o["request_id"] = JsonValue(caller.request_id);
  if (detail.is_object() && !detail.as_object().empty()) o["detail"] = detail;
  o["created_ns"] = JsonValue(created_ns);
  if (finished_ns != 0) o["finished_ns"] = JsonValue(finished_ns);
  return JsonValue(std::move(o));
}

OperationRegistry::OperationRegistry(std::size_t retain_terminal)
    : retain_terminal_(std::max<std::size_t>(1, retain_terminal)) {}

OperationId OperationRegistry::Create(std::string kind, SessionId session, CallerContext caller,
                                      JsonValue detail) {
  std::lock_guard lock(mutex_);
  const OperationId id = next_id_++;
  OperationRecord r;
  r.id = id;
  r.kind = std::move(kind);
  r.session_id = session;
  r.caller = std::move(caller);
  r.detail = detail.is_object() ? std::move(detail) : JsonValue(JsonObject{});
  r.created_ns = WallClockNs();
  records_.emplace(id, std::move(r));
  return id;
}

void OperationRegistry::SetRunning(OperationId id) {
  std::lock_guard lock(mutex_);
  const auto it = records_.find(id);
  if (it == records_.end() || it->second.terminal()) return;
  it->second.state = OperationState::kRunning;
}

void OperationRegistry::SetTopologyVersion(OperationId id, TopologyVersion version) {
  std::lock_guard lock(mutex_);
  const auto it = records_.find(id);
  if (it == records_.end()) return;
  it->second.topology_version = version;
}

void OperationRegistry::Succeed(OperationId id, std::optional<TopologyVersion> version,
                                std::optional<ParameterVersion> parameter_version,
                                JsonValue detail) {
  Finish(id, OperationState::kSucceeded, Status::Ok(), std::move(detail), version,
         parameter_version);
}

void OperationRegistry::Fail(OperationId id, Status status, JsonValue detail) {
  Finish(id, OperationState::kFailed, std::move(status), std::move(detail), std::nullopt,
         std::nullopt);
}

void OperationRegistry::Cancel(OperationId id, std::string reason) {
  Finish(id, OperationState::kCancelled, Status::Cancelled(std::move(reason)), JsonValue(),
         std::nullopt, std::nullopt);
}

void OperationRegistry::Finish(OperationId id, OperationState state, Status result,
                               JsonValue detail, std::optional<TopologyVersion> tv,
                               std::optional<ParameterVersion> pv) {
  std::lock_guard lock(mutex_);
  const auto it = records_.find(id);
  if (it == records_.end() || it->second.terminal()) return;
  OperationRecord& r = it->second;
  r.state = state;
  r.result = std::move(result);
  if (tv) r.topology_version = tv;
  if (pv) r.parameter_version = pv;
  if (detail.is_object()) {
    // JsonValue objects are shared by handle: a record copied out via Get()
    // or Wait() still aliases r.detail, so it is replaced (copy-on-write)
    // rather than mutated in place.
    JsonObject merged = r.detail.is_object() ? r.detail.as_object() : JsonObject{};
    for (const auto& [k, v] : detail.as_object()) merged[k] = v;
    r.detail = JsonValue(std::move(merged));
  }
  r.finished_ns = WallClockNs();
  terminal_order_.push_back(id);
  while (terminal_order_.size() > retain_terminal_) {
    records_.erase(terminal_order_.front());
    terminal_order_.erase(terminal_order_.begin());
  }
  cv_.notify_all();
}

std::optional<OperationRecord> OperationRegistry::Get(OperationId id) const {
  std::lock_guard lock(mutex_);
  const auto it = records_.find(id);
  if (it == records_.end()) return std::nullopt;
  return it->second;
}

std::optional<OperationRecord> OperationRegistry::Wait(OperationId id,
                                                       std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  const auto find = [&]() -> const OperationRecord* {
    const auto it = records_.find(id);
    return it == records_.end() ? nullptr : &it->second;
  };
  const bool done = cv_.wait_for(lock, timeout, [&] {
    const OperationRecord* r = find();
    return r == nullptr || r->terminal();
  });
  if (!done) return std::nullopt;
  const OperationRecord* r = find();
  if (r == nullptr) return std::nullopt;
  return *r;
}

}  // namespace ge
