#ifndef GE_CPP_TYPES_H_
#define GE_CPP_TYPES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <ge/c/ge_abi.h>

namespace ge {

using GraphId = std::uint64_t;
using SessionId = std::uint64_t;
using NodeId = std::uint64_t;
using EdgeId = std::uint64_t;
using PacketSeq = std::uint64_t;
using TopologyVersion = std::uint64_t;
using ParameterVersion = std::uint64_t;
using OperationId = std::uint64_t;
using PluginId = std::uint64_t;
using RequestId = std::uint64_t;

enum class Severity : std::uint8_t { kDebug, kInfo, kWarning, kError };

struct ErrorDetail {
  ge_status_code code = GE_STATUS_OK;
  Severity severity = Severity::kError;
  bool retryable = false;
  std::string message;
  std::string context_json;
};

class Status final {
 public:
  Status() = default;

  Status(ge_status_code code, std::string message, bool retryable = false,
         std::string context_json = {})
      : detail_{code, Severity::kError, retryable, std::move(message),
                std::move(context_json)} {}

  [[nodiscard]] static Status Ok() noexcept { return Status(); }

  [[nodiscard]] static Status InvalidArgument(std::string message) {
    return Status(GE_STATUS_INVALID_ARGUMENT, std::move(message));
  }
  [[nodiscard]] static Status NotFound(std::string message) {
    return Status(GE_STATUS_NOT_FOUND, std::move(message));
  }
  [[nodiscard]] static Status AlreadyExists(std::string message) {
    return Status(GE_STATUS_ALREADY_EXISTS, std::move(message));
  }
  [[nodiscard]] static Status VersionConflict(std::string message) {
    return Status(GE_STATUS_VERSION_CONFLICT, std::move(message), true);
  }
  [[nodiscard]] static Status GraphInvalid(std::string message,
                                           std::string context_json = {}) {
    return Status(GE_STATUS_GRAPH_INVALID, std::move(message), false,
                  std::move(context_json));
  }
  [[nodiscard]] static Status CapabilityConflict(
      std::string message, std::string context_json = {}) {
    return Status(GE_STATUS_CAPABILITY_CONFLICT, std::move(message), false,
                  std::move(context_json));
  }
  [[nodiscard]] static Status SharedDependency(std::string message,
                                               std::string context_json = {}) {
    return Status(GE_STATUS_SHARED_DEPENDENCY, std::move(message), false,
                  std::move(context_json));
  }
  [[nodiscard]] static Status ResourceExhausted(
      std::string message, std::string context_json = {}) {
    return Status(GE_STATUS_RESOURCE_EXHAUSTED, std::move(message), true,
                  std::move(context_json));
  }
  [[nodiscard]] static Status NodeWarmupFailed(std::string message) {
    return Status(GE_STATUS_NODE_WARMUP_FAILED, std::move(message));
  }
  [[nodiscard]] static Status ParameterUnsupported(std::string message) {
    return Status(GE_STATUS_PARAMETER_UNSUPPORTED, std::move(message));
  }
  [[nodiscard]] static Status ParameterApplyFailed(std::string message) {
    return Status(GE_STATUS_PARAMETER_APPLY_FAILED, std::move(message));
  }
  [[nodiscard]] static Status PluginAbiMismatch(std::string message) {
    return Status(GE_STATUS_PLUGIN_ABI_MISMATCH, std::move(message));
  }
  [[nodiscard]] static Status PluginManifestInvalid(std::string message) {
    return Status(GE_STATUS_PLUGIN_MANIFEST_INVALID, std::move(message));
  }
  [[nodiscard]] static Status PluginRetired(std::string message) {
    return Status(GE_STATUS_PLUGIN_RETIRED, std::move(message));
  }
  [[nodiscard]] static Status PluginPhysicalUnloadUnsafe(std::string message) {
    return Status(GE_STATUS_PLUGIN_PHYSICAL_UNLOAD_UNSAFE, std::move(message),
                  true);
  }
  [[nodiscard]] static Status WouldBlock() {
    return Status(GE_STATUS_WOULD_BLOCK, "would block", true);
  }
  [[nodiscard]] static Status Cancelled(std::string message) {
    return Status(GE_STATUS_CANCELLED, std::move(message));
  }
  [[nodiscard]] static Status Internal(std::string message) {
    return Status(GE_STATUS_INTERNAL, std::move(message));
  }

  [[nodiscard]] bool ok() const noexcept {
    return detail_.code == GE_STATUS_OK;
  }
  [[nodiscard]] ge_status_code code() const noexcept { return detail_.code; }
  [[nodiscard]] bool retryable() const noexcept { return detail_.retryable; }
  [[nodiscard]] const std::string& message() const noexcept {
    return detail_.message;
  }
  [[nodiscard]] const std::string& context_json() const noexcept {
    return detail_.context_json;
  }
  [[nodiscard]] const ErrorDetail& detail() const noexcept { return detail_; }

  [[nodiscard]] std::string ToString() const {
    std::string out(CodeName(detail_.code));
    if (!detail_.message.empty()) {
      out += ": ";
      out += detail_.message;
    }
    return out;
  }

  [[nodiscard]] static std::string_view CodeName(ge_status_code code) noexcept {
    switch (code) {
      case GE_STATUS_OK: return "OK";
      case GE_STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
      case GE_STATUS_NOT_FOUND: return "NOT_FOUND";
      case GE_STATUS_ALREADY_EXISTS: return "ALREADY_EXISTS";
      case GE_STATUS_VERSION_CONFLICT: return "VERSION_CONFLICT";
      case GE_STATUS_GRAPH_INVALID: return "GRAPH_INVALID";
      case GE_STATUS_CAPABILITY_CONFLICT: return "CAPABILITY_CONFLICT";
      case GE_STATUS_SHARED_DEPENDENCY: return "SHARED_DEPENDENCY";
      case GE_STATUS_RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
      case GE_STATUS_NODE_WARMUP_FAILED: return "NODE_WARMUP_FAILED";
      case GE_STATUS_PARAMETER_UNSUPPORTED: return "PARAMETER_UNSUPPORTED";
      case GE_STATUS_PARAMETER_APPLY_FAILED: return "PARAMETER_APPLY_FAILED";
      case GE_STATUS_PLUGIN_ABI_MISMATCH: return "PLUGIN_ABI_MISMATCH";
      case GE_STATUS_PLUGIN_MANIFEST_INVALID: return "PLUGIN_MANIFEST_INVALID";
      case GE_STATUS_PLUGIN_RETIRED: return "PLUGIN_RETIRED";
      case GE_STATUS_PLUGIN_PHYSICAL_UNLOAD_UNSAFE:
        return "PLUGIN_PHYSICAL_UNLOAD_UNSAFE";
      case GE_STATUS_WOULD_BLOCK: return "WOULD_BLOCK";
      case GE_STATUS_CANCELLED: return "CANCELLED";
      case GE_STATUS_INTERNAL: return "INTERNAL";
    }
    return "UNKNOWN";
  }

 private:
  ErrorDetail detail_;
};

template <typename T>
class Result final {
 public:
  Result(T value) : data_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Status status) : data_(std::move(status)) {  // NOLINT(google-explicit-constructor)
    if (std::get<Status>(data_).ok()) {
      data_ = Status::Internal("Result constructed from OK status without value");
    }
  }

  [[nodiscard]] bool ok() const noexcept { return data_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Status& status() const noexcept {
    static const Status kOk;
    return ok() ? kOk : std::get<Status>(data_);
  }

  [[nodiscard]] T& value() & { return std::get<T>(data_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(data_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(data_)); }

  [[nodiscard]] T& operator*() & { return value(); }
  [[nodiscard]] const T& operator*() const& { return value(); }
  [[nodiscard]] T* operator->() { return &value(); }
  [[nodiscard]] const T* operator->() const { return &value(); }

 private:
  std::variant<T, Status> data_;
};

template <>
class Result<void> final {
 public:
  Result() = default;
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  Status status_;
};

struct CallerContext {
  std::string caller_id;
  std::string request_id;
};

// Identifies an operator implementation as "type@version".
struct OperatorKey {
  std::string type_name;
  std::string semantic_version;

  [[nodiscard]] std::string ToString() const {
    return type_name + "@" + semantic_version;
  }
  [[nodiscard]] static std::optional<OperatorKey> Parse(std::string_view text) {
    const auto at = text.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= text.size()) {
      return std::nullopt;
    }
    return OperatorKey{std::string(text.substr(0, at)),
                       std::string(text.substr(at + 1))};
  }
  friend bool operator==(const OperatorKey&, const OperatorKey&) = default;
  friend auto operator<=>(const OperatorKey&, const OperatorKey&) = default;
};

}  // namespace ge

#endif
