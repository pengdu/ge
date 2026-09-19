#ifndef GE_CPP_OPERATOR_H_
#define GE_CPP_OPERATOR_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ge/c/ge_plugin.h>
#include <ge/cpp/capability.h>
#include <ge/cpp/graph_validator.h>
#include <ge/cpp/packet.h>
#include <ge/cpp/types.h>

namespace ge {

// Engine-side operator contract mirroring the C ABI vtable (13 §4.4–4.5).
// In-process test operators implement this directly; P4 adapts C plugins.
// Everything here runs on an executor thread; the engine guarantees no two
// invocations of one instance overlap unless max_parallelism > 1.

// Negotiated contract of one port (12 §8.2), handed to builtin operators at
// Open so they can pick the agreed pixel/sample format. C++ only; C plugins
// keep receiving the contract through their capability JSON (P6).
struct PortContract {
  std::string port;  // owned: the topology's port lists are returned by value
  const ConnectionContract* contract = nullptr;
};

struct OpenRequest {
  SessionId session_id = 0;
  TopologyVersion topology_version = 0;
  const JsonValue* options = nullptr;
  // P6 tail fields (append-only).
  std::vector<PortContract> input_contracts;
  std::vector<PortContract> output_contracts;
  NodeId node_id = 0;
  std::string_view external_id;

  [[nodiscard]] const ConnectionContract* InputContract(std::string_view port) const noexcept {
    for (const PortContract& c : input_contracts) if (c.port == port) return c.contract;
    return nullptr;
  }
  [[nodiscard]] const ConnectionContract* OutputContract(std::string_view port) const noexcept {
    for (const PortContract& c : output_contracts) if (c.port == port) return c.contract;
    return nullptr;
  }
};

struct CloseRequest {
  SessionId session_id = 0;
  TopologyVersion topology_version = 0;
  bool fast_shutdown = false;
};

// Emits are only valid while the process call that received the context is
// on the stack (13 §4.5).
class EmitSink {
 public:
  virtual ~EmitSink() = default;
  virtual Status Emit(std::string_view output_port, Packet packet) = 0;
};

// Runtime event publication for builtin operators (12 §12.2); the C ABI
// equivalent is ge_host_services.event_publish. Events land on the
// engine EventBus attributed to the calling node.
class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void Publish(std::string_view type, Severity severity, JsonValue detail) = 0;
};

struct ProcessRequest {
  SessionId session_id = 0;
  TopologyVersion topology_version = 0;
  ParameterVersion parameter_version = 0;
  std::uint32_t flags = 0;  // GE_PROCESS_FLAG_*
  std::vector<std::string_view> input_ports;
  std::vector<PacketRef> inputs;  // parallel to input_ports
  const JsonValue* parameters = nullptr;
  EmitSink* sink = nullptr;
  // P6 tail field (append-only): may be null for FLUSH from a retired path.
  EventSink* events = nullptr;
};

// Source semantics (12 §4.4 step 1): a node without inputs is invoked with
// input_count == 0 and no FLUSH flag; it returns kExhausted when it will not
// produce again, after which the scheduler sends EOS on its outputs.
enum class ProcessResult : std::uint8_t {
  kContinue,   // normal
  kExhausted,  // source only: no more data
};

// ---------------------------------------------------------------------------
// Asynchronous contract (12 §3.5–3.6, 13 §4.6). Submit must return without
// waiting for the backend; the result arrives later through
// CompletionSink::Push from any thread. One completion per request_id.
// ---------------------------------------------------------------------------

struct CompletionOutput {
  std::string output_port;
  Packet packet;
};

struct CompletionEvent {
  RequestId request_id = 0;
  SessionId session_id = 0;
  TopologyVersion topology_version = 0;
  ParameterVersion parameter_version = 0;
  PacketSeq packet_seq = 0;
  Status status;
  std::vector<CompletionOutput> outputs;
};

// Owned by the Session (one per session, 12 §3.5); valid for the operator
// until Close returned and every submitted request completed.
class CompletionSink {
 public:
  virtual ~CompletionSink() = default;
  // RESOURCE_EXHAUSTED when the queue is full (the event is dropped and
  // counted); the plugin should treat that as a failed request.
  virtual Status Push(CompletionEvent event) = 0;
};

struct SubmitRequest {
  RequestId request_id = 0;
  SessionId session_id = 0;
  TopologyVersion topology_version = 0;
  ParameterVersion parameter_version = 0;
  PacketSeq packet_seq = 0;
  std::vector<std::string_view> input_ports;
  std::vector<PacketRef> inputs;  // parallel to input_ports
  const JsonValue* parameters = nullptr;
  CompletionSink* completion_sink = nullptr;
  // Batch framing (ASY-5/6): members of one batch are submitted back to
  // back on one thread; batch_index runs 0..batch_size-1.
  std::uint64_t batch_id = 0;
  std::uint32_t batch_index = 0;
  std::uint32_t batch_size = 1;
};

class Operator {
 public:
  virtual ~Operator() = default;
  virtual Status Open(const OpenRequest& request) = 0;
  virtual Result<ProcessResult> Process(const ProcessRequest& request) = 0;
  // Async operators (capability execution.async) receive data here instead
  // of Process; Process is still used for FLUSH. Default: unsupported.
  virtual Status Submit(const SubmitRequest& request);
  virtual Status Close(const CloseRequest& request) = 0;
};

struct OperatorCreateArgs {
  OperatorKey key;
  NodeId node_id = 0;
  std::string external_id;
  JsonValue options = JsonValue(JsonObject{});
  SessionId session_id = 0;
  TopologyVersion topology_version = 0;
};

// Creates in-process operators for a given key. Backed by PluginRegistry
// from P4 on; tests/samples register builtin factories.
class OperatorFactory {
 public:
  virtual ~OperatorFactory() = default;
  [[nodiscard]] virtual const CapabilityDescriptor* Describe(const OperatorKey& key) const = 0;
  [[nodiscard]] virtual Result<std::unique_ptr<Operator>> Create(const OperatorCreateArgs& args) = 0;
};

// Simple in-memory factory for builtin/test operators.
class BuiltinOperatorFactory final : public OperatorFactory {
 public:
  using Maker = std::function<std::unique_ptr<Operator>(const OperatorCreateArgs&)>;
  void Register(CapabilityDescriptor descriptor, Maker maker);
  [[nodiscard]] const CapabilityDescriptor* Describe(const OperatorKey& key) const override;
  [[nodiscard]] Result<std::unique_ptr<Operator>> Create(const OperatorCreateArgs& args) override;
  [[nodiscard]] CapabilityResolver resolver() const {
    return [this](const OperatorKey& k) { return Describe(k); };
  }

 private:
  struct Entry {
    CapabilityDescriptor descriptor;
    Maker maker;
  };
  std::vector<Entry> entries_;
};

// Ordered lookup over several factories (13 §6.1): the engine puts the
// host's builtin operators first and the PluginRegistry last, so a builtin
// key always wins over a plugin that happens to declare the same one.
class CompositeOperatorFactory final : public OperatorFactory {
 public:
  void Add(OperatorFactory* factory) { factories_.push_back(factory); }
  [[nodiscard]] const CapabilityDescriptor* Describe(const OperatorKey& key) const override;
  [[nodiscard]] Result<std::unique_ptr<Operator>> Create(const OperatorCreateArgs& args) override;
  [[nodiscard]] CapabilityResolver resolver() const {
    return [this](const OperatorKey& k) { return Describe(k); };
  }

 private:
  std::vector<OperatorFactory*> factories_;
};

}  // namespace ge

#endif
