#ifndef GE_CPP_PLUGIN_OPERATOR_H_
#define GE_CPP_PLUGIN_OPERATOR_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ge/c/ge_plugin.h>
#include <ge/cpp/operator.h>
#include <ge/cpp/packet.h>
#include <ge/cpp/types.h>

namespace ge {

// Host-side services a PluginOperator reaches through ge_host_api that are
// not tied to one process call: buffer pool, observation events and logs.
struct HostServices {
  std::shared_ptr<HostBufferPool> pool;
  std::function<void(const ge_event& event)> event_publish;
  std::function<void(const ge_log_record& record)> log;
};

// ---------------------------------------------------------------------------
// HostApiAdapter (12 §12.4, 13 §7.2): one per (Session, NodeRuntime). The
// static ge_host_api function table recovers the adapter from host_context
// (create/open/close) or from emit_args.callback_context (process). Emit is
// only honoured while the process call that received the context is on the
// stack.
// ---------------------------------------------------------------------------

class HostApiAdapter final {
 public:
  HostApiAdapter(SessionId session, NodeId node, std::string external_id, bool is_source,
                 HostServices services);
  ~HostApiAdapter();
  HostApiAdapter(const HostApiAdapter&) = delete;
  HostApiAdapter& operator=(const HostApiAdapter&) = delete;

  [[nodiscard]] static const ge_host_api& vtable() noexcept;
  [[nodiscard]] void* host_context() noexcept { return this; }

  // Scope of one synchronous process call.
  struct InvokeScope {
    InvokeScope(HostApiAdapter& adapter, EmitSink& sink) noexcept;
    ~InvokeScope();
    InvokeScope(const InvokeScope&) = delete;
    InvokeScope& operator=(const InvokeScope&) = delete;
    [[nodiscard]] void* callback_context() const noexcept { return context_; }
    [[nodiscard]] bool exhausted() const noexcept;

   private:
    HostApiAdapter& adapter_;
    void* context_;
  };

  // Marshals a Packet into a view that stays valid while |keep| lives.
  struct ViewHolder {
    std::string type_tag;
    std::string metadata_json;
    ge_packet_view view{};
  };
  static void Fill(const Packet& packet, ViewHolder* holder);

 private:
  static HostApiAdapter* From(void* host_context) noexcept;
  static ge_status BufferRetain(ge_buffer_handle buffer);
  static void BufferRelease(ge_buffer_handle buffer);
  static ge_status BufferGetView(ge_buffer_handle buffer, ge_buffer_view* out_view);
  static ge_status Emit(const ge_emit_args* args);
  // 12 §3.5: any thread; converts the C event (payloads retained by the
  // plugin, adopted here) into a CompletionEvent for the session's sink.
  static ge_status CompletionPush(ge_completion_sink_handle sink, const ge_completion_event* event);
  static ge_status EventPublish(const ge_event* event);
  static void Log(const ge_log_record* record);
  static ge_status BufferAlloc(void* host_context, size_t size, ge_memory_kind memory_kind,
                               int32_t device_id, ge_buffer_handle* out_buffer);
  static ge_status BufferWrap(void* host_context, const ge_buffer_view* view,
                              ge_buffer_release_fn release, void* release_context,
                              ge_buffer_handle* out_buffer);
  Status DoEmit(std::string_view port, const ge_packet_view& view);

  SessionId session_;
  std::string external_id_;
  bool is_source_;
  HostServices services_;
  void* current_ = nullptr;  // Invoke*, set only inside InvokeScope, same thread
  std::uint64_t magic_;
};

// ---------------------------------------------------------------------------
// PluginOperator: Operator implemented by a C plugin vtable (13 §4.4–4.7).
// Owns the ge_operator_handle; destroy runs from the destructor exactly
// once. The |lease| keeps the plugin registered until the last instance is
// gone (12 §9.3).
// ---------------------------------------------------------------------------

class PluginOperator final : public Operator {
 public:
  [[nodiscard]] static Result<std::unique_ptr<PluginOperator>> Create(
      const ge_operator_vtable& vtable, const OperatorCreateArgs& args, bool is_source,
      HostServices services, std::shared_ptr<void> lease);
  ~PluginOperator() override;

  Status Open(const OpenRequest& request) override;
  Result<ProcessResult> Process(const ProcessRequest& request) override;
  // 13 §4.6: marshals the request and calls vtable.submit. Input payloads
  // stay valid for the call only; the plugin retains what it keeps.
  Status Submit(const SubmitRequest& request) override;
  Status Close(const CloseRequest& request) override;

  [[nodiscard]] ge_operator_handle handle() const noexcept { return handle_; }

 private:
  PluginOperator(const ge_operator_vtable& vtable, std::unique_ptr<HostApiAdapter> adapter,
                 std::shared_ptr<void> lease);

  const ge_operator_vtable& vtable_;
  std::unique_ptr<HostApiAdapter> adapter_;
  std::shared_ptr<void> lease_;
  ge_operator_handle handle_ = nullptr;
  bool opened_ = false;
  bool closed_ = false;
};

// ge_status <-> Status. The C message/detail pointers must outlive the call;
// FromC copies them.
[[nodiscard]] Status StatusFromC(const ge_status& status);
// Fills |out| with pointers into |storage| (which must outlive the use).
struct CStatusStorage {
  std::string message;
  std::string detail;
};
void StatusToC(const Status& status, CStatusStorage* storage, ge_status* out) noexcept;
[[nodiscard]] ge_status OkStatus() noexcept;

}  // namespace ge

#endif
