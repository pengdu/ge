#include <ge/cpp/plugin_operator.h>

#include <cstring>
#include <map>
#include <mutex>

#include <ge/cpp/async_runtime.h>

namespace ge {

namespace {

constexpr std::uint64_t kAdapterMagic = 0x47454841444150ull;  // "GEHADAP"

struct Invoke {
  HostApiAdapter* adapter = nullptr;
  EmitSink* sink = nullptr;
  bool exhausted = false;
};

// 12 §12.4: the current synchronous call of this thread. emit() is only
// honoured when callback_context names exactly this object, which rejects
// stale contexts (process returned) and cross-thread misuse without ever
// dereferencing a possibly dangling pointer.
thread_local Invoke* g_current_invoke = nullptr;

template <typename T>
bool HeaderOk(const T* s, std::size_t min_size) noexcept {
  return s != nullptr && s->header.abi_major == GE_ABI_MAJOR && s->header.struct_size >= min_size;
}

// log()/event_publish() carry no host_context; they resolve the session's
// services through this table (13 §5.4 ge_log_record.session_id).
class SessionServices final {
 public:
  static SessionServices& Global() {
    static SessionServices* g = new SessionServices;
    return *g;
  }
  void Add(SessionId session, const HostServices& services) {
    std::lock_guard lock(mutex_);
    Entry& e = entries_[session];
    if (e.refs++ == 0) e.services = services;
  }
  void Remove(SessionId session) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(session);
    if (it == entries_.end()) return;
    if (--it->second.refs == 0) entries_.erase(it);
  }
  HostServices Find(SessionId session) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(session);
    return it == entries_.end() ? HostServices{} : it->second.services;
  }

 private:
  struct Entry {
    HostServices services;
    std::uint32_t refs = 0;
  };
  std::mutex mutex_;
  std::map<SessionId, Entry> entries_;
};

CStatusStorage& ThreadStatusStorage() {
  thread_local CStatusStorage storage;
  return storage;
}

ge_status MakeStatus(const Status& s) noexcept {
  ge_status out;
  StatusToC(s, &ThreadStatusStorage(), &out);
  return out;
}

struct WrapContext {
  ge_buffer_release_fn release;
  void* context;
};

void WrapDeleter(Buffer* buffer, void* context) {
  auto* wc = static_cast<WrapContext*>(context);
  if (wc->release != nullptr) wc->release(buffer->data, buffer->size, wc->context);
  delete wc;
}

}  // namespace

// ---------------------------------------------------------------------------
// Status bridging
// ---------------------------------------------------------------------------

Status StatusFromC(const ge_status& status) {
  if (status.code == GE_STATUS_OK) return Status::Ok();
  return Status(status.code, status.message != nullptr ? status.message : "",
                status.retryable != 0, status.detail_json != nullptr ? status.detail_json : "");
}

void StatusToC(const Status& status, CStatusStorage* storage, ge_status* out) noexcept {
  *out = ge_status{};
  out->header.struct_size = sizeof(ge_status);
  out->header.abi_major = GE_ABI_MAJOR;
  out->code = status.code();
  out->retryable = status.retryable() ? 1 : 0;
  storage->message = status.message();
  storage->detail = status.context_json();
  out->message = storage->message.c_str();
  out->detail_json = storage->detail.empty() ? nullptr : storage->detail.c_str();
}

ge_status OkStatus() noexcept {
  ge_status s{};
  s.header.struct_size = sizeof(ge_status);
  s.header.abi_major = GE_ABI_MAJOR;
  s.code = GE_STATUS_OK;
  return s;
}

// ---------------------------------------------------------------------------
// HostApiAdapter
// ---------------------------------------------------------------------------

HostApiAdapter::HostApiAdapter(SessionId session, NodeId /*node*/, std::string external_id,
                               bool is_source, HostServices services)
    : session_(session),
      external_id_(std::move(external_id)),
      is_source_(is_source),
      services_(std::move(services)),
      magic_(kAdapterMagic) {
  SessionServices::Global().Add(session_, services_);
}

HostApiAdapter::~HostApiAdapter() {
  magic_ = 0;
  SessionServices::Global().Remove(session_);
}

const ge_host_api& HostApiAdapter::vtable() noexcept {
  static const ge_host_api kVtable = [] {
    ge_host_api v{};
    v.header.struct_size = sizeof(ge_host_api);
    v.header.abi_major = GE_ABI_MAJOR;
    v.buffer_retain = &BufferRetain;
    v.buffer_release = &BufferRelease;
    v.buffer_get_view = &BufferGetView;
    v.emit = &Emit;
    v.completion_push = &CompletionPush;
    v.event_publish = &EventPublish;
    v.log = &Log;
    v.buffer_alloc = &BufferAlloc;
    v.buffer_wrap = &BufferWrap;
    return v;
  }();
  return kVtable;
}

HostApiAdapter* HostApiAdapter::From(void* host_context) noexcept {
  auto* a = static_cast<HostApiAdapter*>(host_context);
  return (a != nullptr && a->magic_ == kAdapterMagic) ? a : nullptr;
}

HostApiAdapter::InvokeScope::InvokeScope(HostApiAdapter& adapter, EmitSink& sink) noexcept
    : adapter_(adapter), context_(new Invoke{&adapter, &sink, false}) {
  adapter_.current_ = context_;
  g_current_invoke = static_cast<Invoke*>(context_);
}

HostApiAdapter::InvokeScope::~InvokeScope() {
  g_current_invoke = nullptr;
  adapter_.current_ = nullptr;
  delete static_cast<Invoke*>(context_);
}

bool HostApiAdapter::InvokeScope::exhausted() const noexcept {
  return static_cast<Invoke*>(context_)->exhausted;
}

void HostApiAdapter::Fill(const Packet& packet, ViewHolder* holder) {
  holder->type_tag = std::string(TypeTagRegistry::Global().Name(packet.header.type_tag));
  holder->metadata_json = packet.metadata ? packet.metadata->ToJson() : std::string();
  ge_packet_view& v = holder->view;
  v = ge_packet_view{};
  v.header.struct_size = sizeof(ge_packet_view);
  v.header.abi_major = GE_ABI_MAJOR;
  v.seq = packet.header.seq;
  v.pts_ns = packet.header.pts_ns;
  v.dts_ns = packet.header.dts_ns;
  v.flags = packet.header.flags;
  v.topology_version = packet.header.topology_version;
  v.parameter_version = packet.header.parameter_version;
  v.type_tag = holder->type_tag.c_str();
  v.metadata_json = holder->metadata_json.empty() ? nullptr : holder->metadata_json.c_str();
  v.payload = packet.payload.handle();
}

ge_status HostApiAdapter::BufferRetain(ge_buffer_handle buffer) {
  if (buffer == nullptr) return MakeStatus(Status::InvalidArgument("null buffer"));
  Buffer* b = reinterpret_cast<Buffer*>(buffer);
  b->refs.fetch_add(1, std::memory_order_relaxed);
  return OkStatus();
}

void HostApiAdapter::BufferRelease(ge_buffer_handle buffer) {
  if (buffer == nullptr) return;
  BufferRef::Adopt(reinterpret_cast<Buffer*>(buffer)).Reset();
}

ge_status HostApiAdapter::BufferGetView(ge_buffer_handle buffer, ge_buffer_view* out_view) {
  if (buffer == nullptr || out_view == nullptr) {
    return MakeStatus(Status::InvalidArgument("null buffer or view"));
  }
  *out_view = reinterpret_cast<const Buffer*>(buffer)->View();
  return OkStatus();
}

ge_status HostApiAdapter::BufferAlloc(void* host_context, size_t size, ge_memory_kind memory_kind,
                                      int32_t device_id, ge_buffer_handle* out_buffer) {
  HostApiAdapter* a = From(host_context);
  if (a == nullptr || out_buffer == nullptr) {
    return MakeStatus(Status::InvalidArgument("invalid host_context"));
  }
  if (memory_kind != GE_MEMORY_HOST || device_id != -1) {
    return MakeStatus(Status::InvalidArgument("only host memory can be allocated by the engine"));
  }
  if (!a->services_.pool) return MakeStatus(Status::ResourceExhausted("no buffer pool"));
  BufferRef ref = a->services_.pool->Allocate(size);
  if (!ref) return MakeStatus(Status::ResourceExhausted("buffer allocation failed"));
  *out_buffer = reinterpret_cast<ge_buffer_handle>(ref.Release());
  return OkStatus();
}

ge_status HostApiAdapter::BufferWrap(void* host_context, const ge_buffer_view* view,
                                     ge_buffer_release_fn release, void* release_context,
                                     ge_buffer_handle* out_buffer) {
  if (From(host_context) == nullptr || out_buffer == nullptr ||
      !HeaderOk(view, sizeof(ge_buffer_view))) {
    return MakeStatus(Status::InvalidArgument("invalid host_context or view"));
  }
  if (view->data == nullptr && view->size != 0) {
    return MakeStatus(Status::InvalidArgument("null data with non-zero size"));
  }
  if (view->memory_kind < GE_MEMORY_HOST || view->memory_kind > GE_MEMORY_DMABUF) {
    return MakeStatus(Status::InvalidArgument("unknown memory kind"));
  }
  BufferRef ref = WrapExternalBuffer(view->data, view->size,
                                     static_cast<MemoryKind>(view->memory_kind), view->device_id,
                                     &WrapDeleter, new WrapContext{release, release_context});
  *out_buffer = reinterpret_cast<ge_buffer_handle>(ref.Release());
  return OkStatus();
}

ge_status HostApiAdapter::Emit(const ge_emit_args* args) {
  if (!HeaderOk(args, sizeof(ge_emit_args))) {
    return MakeStatus(Status::InvalidArgument("invalid emit args"));
  }
  Invoke* inv = g_current_invoke;
  if (inv == nullptr || args->callback_context != inv) {
    return MakeStatus(Status::InvalidArgument("emit outside of process or wrong callback_context"));
  }
  if (args->output_port == nullptr || !HeaderOk(args->packet, sizeof(ge_packet_view))) {
    return MakeStatus(Status::InvalidArgument("emit needs output_port and packet"));
  }
  return MakeStatus(inv->adapter->DoEmit(args->output_port, *args->packet));
}

Status HostApiAdapter::DoEmit(std::string_view port, const ge_packet_view& view) {
  Invoke* inv = static_cast<Invoke*>(current_);
  if (inv == nullptr || inv->sink == nullptr) return Status::InvalidArgument("no active process");
  if ((view.flags & GE_PACKET_FLAG_EOS) != 0) {
    // 13 §4.5: a source reports exhaustion by emitting an empty EOS packet;
    // the scheduler injects EOS on every output (12 §4.4 step 1).
    if (!is_source_) {
      return Status::InvalidArgument("node '" + external_id_ + "' may not emit EOS");
    }
    if (view.payload != nullptr) return Status::InvalidArgument("EOS packet must have no payload");
    inv->exhausted = true;
    return Status::Ok();
  }
  if ((view.flags & GE_PACKET_FLAG_DROPPED) != 0) {
    return Status::InvalidArgument("plugins may not emit DROPPED placeholders");
  }
  Packet p;
  p.header.seq = view.seq;
  p.header.pts_ns = view.pts_ns;
  p.header.dts_ns = view.dts_ns;
  p.header.flags = view.flags;
  if (view.type_tag == nullptr || view.type_tag[0] == '\0') {
    return Status::InvalidArgument("packet type_tag is required");
  }
  p.header.type_tag = TypeTagRegistry::Global().Intern(view.type_tag);
  if (view.metadata_json != nullptr && view.metadata_json[0] != '\0') {
    auto m = Metadata::FromJson(view.metadata_json);
    if (!m.ok()) return m.status();
    p.metadata = std::make_shared<const Metadata>(std::move(*m));
  }
  if (view.payload != nullptr) p.payload = BufferRef::FromHandle(view.payload);
  return inv->sink->Emit(port, std::move(p));
}

namespace {

// Builds an engine Packet from a plugin view. The plugin handed us one
// reference per payload (13 §4.6 "retain 后交付"); we adopt it so the
// buffer is released exactly once, after routing.
Status PacketFromView(const ge_packet_view& view, Packet* out) {
  if (!HeaderOk(&view, sizeof(ge_packet_view))) return Status::InvalidArgument("invalid packet view");
  if ((view.flags & (GE_PACKET_FLAG_EOS | GE_PACKET_FLAG_DROPPED)) != 0) {
    return Status::InvalidArgument("completion outputs may not carry EOS/DROPPED flags");
  }
  if (view.type_tag == nullptr || view.type_tag[0] == '\0') {
    return Status::InvalidArgument("packet type_tag is required");
  }
  Packet p;
  p.header.seq = view.seq;
  p.header.pts_ns = view.pts_ns;
  p.header.dts_ns = view.dts_ns;
  p.header.flags = view.flags;
  p.header.type_tag = TypeTagRegistry::Global().Intern(view.type_tag);
  if (view.metadata_json != nullptr && view.metadata_json[0] != '\0') {
    auto m = Metadata::FromJson(view.metadata_json);
    if (!m.ok()) return m.status();
    p.metadata = std::make_shared<const Metadata>(std::move(*m));
  }
  if (view.payload != nullptr) p.payload = BufferRef::Adopt(reinterpret_cast<Buffer*>(view.payload));
  *out = std::move(p);
  return Status::Ok();
}

}  // namespace

ge_status HostApiAdapter::CompletionPush(ge_completion_sink_handle sink,
                                         const ge_completion_event* event) {
  SessionCompletionSink* s = SessionCompletionSink::FromHandle(sink);
  if (s == nullptr) return MakeStatus(Status::InvalidArgument("invalid or closed completion sink"));
  if (!HeaderOk(event, sizeof(ge_completion_event))) {
    return MakeStatus(Status::InvalidArgument("invalid completion event"));
  }
  if (event->output_count != 0 && (event->outputs == nullptr || event->output_ports == nullptr)) {
    return MakeStatus(Status::InvalidArgument("completion outputs without ports"));
  }
  CompletionEvent ev;
  ev.request_id = event->request_id;
  ev.session_id = event->session_id;
  ev.topology_version = event->topology_version;
  ev.parameter_version = event->parameter_version;
  ev.packet_seq = event->packet_seq;
  ev.status = StatusFromC(event->status);
  ev.outputs.reserve(event->output_count);
  Status bad;
  for (uint32_t i = 0; i < event->output_count; ++i) {
    CompletionOutput out;
    out.output_port = event->output_ports[i] != nullptr ? event->output_ports[i] : "";
    if (Status st = PacketFromView(event->outputs[i], &out.packet); !st.ok()) {
      if (bad.ok()) bad = st;
      continue;  // payloads of later outputs are still adopted below
    }
    ev.outputs.push_back(std::move(out));
  }
  if (!bad.ok()) {
    // Reject the whole event but never leak the buffers the plugin handed
    // over: adopt and drop the rest.
    for (uint32_t i = 0; i < event->output_count; ++i) {
      bool adopted = false;
      for (const CompletionOutput& o : ev.outputs) {
        if (o.packet.payload && o.packet.payload.handle() == event->outputs[i].payload) adopted = true;
      }
      if (!adopted && event->outputs[i].payload != nullptr) {
        BufferRef::Adopt(reinterpret_cast<Buffer*>(event->outputs[i].payload)).Reset();
      }
    }
    return MakeStatus(bad);
  }
  return MakeStatus(s->Push(std::move(ev)));
}

ge_status HostApiAdapter::EventPublish(const ge_event* event) {
  if (!HeaderOk(event, sizeof(ge_event)) || event->type == nullptr) {
    return MakeStatus(Status::InvalidArgument("invalid event"));
  }
  const HostServices services = SessionServices::Global().Find(event->session_id);
  if (!services.event_publish) return MakeStatus(Status::NotFound("no event sink for session"));
  services.event_publish(*event);
  return OkStatus();
}

void HostApiAdapter::Log(const ge_log_record* record) {
  if (!HeaderOk(record, sizeof(ge_log_record)) || record->message == nullptr) return;
  const HostServices services = SessionServices::Global().Find(record->session_id);
  if (services.log) services.log(*record);
}

// ---------------------------------------------------------------------------
// PluginOperator
// ---------------------------------------------------------------------------

PluginOperator::PluginOperator(const ge_operator_vtable& vtable,
                               std::unique_ptr<HostApiAdapter> adapter, std::shared_ptr<void> lease)
    : vtable_(vtable), adapter_(std::move(adapter)), lease_(std::move(lease)) {}

Result<std::unique_ptr<PluginOperator>> PluginOperator::Create(const ge_operator_vtable& vtable,
                                                               const OperatorCreateArgs& args,
                                                               bool is_source,
                                                               HostServices services,
                                                               std::shared_ptr<void> lease) {
  if (vtable.create == nullptr || vtable.open == nullptr || vtable.process == nullptr ||
      vtable.close == nullptr || vtable.destroy == nullptr) {
    return Status::PluginAbiMismatch("operator vtable is missing mandatory entries");
  }
  auto adapter = std::make_unique<HostApiAdapter>(args.session_id, args.node_id, args.external_id,
                                                  is_source, std::move(services));
  const std::string key = args.key.ToString();
  const std::string options = args.options.Serialize();
  ge_operator_create_args cargs{};
  cargs.header.struct_size = sizeof(ge_operator_create_args);
  cargs.header.abi_major = GE_ABI_MAJOR;
  cargs.operator_key = key.c_str();
  cargs.node_id = args.node_id;
  cargs.options_json = options.c_str();
  cargs.host_api = &HostApiAdapter::vtable();
  cargs.host_context = adapter->host_context();
  ge_operator_handle handle = nullptr;
  const ge_status st = vtable.create(&cargs, &handle);
  if (st.code != GE_STATUS_OK) return StatusFromC(st);
  if (handle == nullptr) return Status::PluginAbiMismatch("operator create returned a null handle");
  std::unique_ptr<PluginOperator> op(new PluginOperator(vtable, std::move(adapter), std::move(lease)));
  op->handle_ = handle;
  return op;
}

PluginOperator::~PluginOperator() {
  if (handle_ == nullptr) return;
  if (opened_ && !closed_) {
    ge_close_request req{};
    req.header.struct_size = sizeof(ge_close_request);
    req.header.abi_major = GE_ABI_MAJOR;
    req.fast_shutdown = 1;
    (void)vtable_.close(handle_, &req);
  }
  vtable_.destroy(handle_);
  handle_ = nullptr;
}

Status PluginOperator::Open(const OpenRequest& request) {
  const std::string options = request.options != nullptr ? request.options->Serialize() : "{}";
  ge_open_request req{};
  req.header.struct_size = sizeof(ge_open_request);
  req.header.abi_major = GE_ABI_MAJOR;
  req.session_id = request.session_id;
  req.topology_version = request.topology_version;
  req.options_json = options.c_str();
  const ge_status st = vtable_.open(handle_, &req);
  if (st.code != GE_STATUS_OK) return StatusFromC(st);
  opened_ = true;
  return Status::Ok();
}

Result<ProcessResult> PluginOperator::Process(const ProcessRequest& request) {
  if (request.sink == nullptr) return Status::Internal("process without emit sink");
  const std::size_t n = request.inputs.size();
  std::vector<HostApiAdapter::ViewHolder> holders(n);
  std::vector<ge_packet_view> views(n);
  std::vector<std::string> port_storage(n);
  std::vector<const char*> ports(n);
  for (std::size_t i = 0; i < n; ++i) {
    HostApiAdapter::Fill(*request.inputs[i], &holders[i]);
    views[i] = holders[i].view;
    port_storage[i] = std::string(request.input_ports[i]);
    ports[i] = port_storage[i].c_str();
  }
  const std::string parameters = request.parameters != nullptr ? request.parameters->Serialize() : "{}";
  HostApiAdapter::InvokeScope scope(*adapter_, *request.sink);
  ge_process_request req{};
  req.header.struct_size = sizeof(ge_process_request);
  req.header.abi_major = GE_ABI_MAJOR;
  req.session_id = request.session_id;
  req.topology_version = request.topology_version;
  req.parameter_version = request.parameter_version;
  req.flags = request.flags;
  req.inputs = n == 0 ? nullptr : views.data();
  req.input_ports = n == 0 ? nullptr : ports.data();
  req.input_count = static_cast<uint32_t>(n);
  req.parameter_json = parameters.c_str();
  req.callback_context = scope.callback_context();
  const ge_status st = vtable_.process(handle_, &req);
  if (st.code != GE_STATUS_OK) return StatusFromC(st);
  return scope.exhausted() ? ProcessResult::kExhausted : ProcessResult::kContinue;
}

Status PluginOperator::Submit(const SubmitRequest& request) {
  if (vtable_.submit == nullptr) return Status::InvalidArgument("operator has no submit entry");
  if (request.completion_sink == nullptr) return Status::Internal("submit without completion sink");
  auto* sink = dynamic_cast<SessionCompletionSink*>(request.completion_sink);
  if (sink == nullptr) return Status::Internal("completion sink is not a session sink");
  const std::size_t n = request.inputs.size();
  std::vector<HostApiAdapter::ViewHolder> holders(n);
  std::vector<ge_packet_view> views(n);
  std::vector<std::string> port_storage(n);
  std::vector<const char*> ports(n);
  for (std::size_t i = 0; i < n; ++i) {
    HostApiAdapter::Fill(*request.inputs[i], &holders[i]);
    views[i] = holders[i].view;
    port_storage[i] = std::string(request.input_ports[i]);
    ports[i] = port_storage[i].c_str();
  }
  const std::string parameters = request.parameters != nullptr ? request.parameters->Serialize() : "{}";
  ge_submit_request req{};
  req.header.struct_size = sizeof(ge_submit_request);
  req.header.abi_major = GE_ABI_MAJOR;
  req.request_id = request.request_id;
  req.session_id = request.session_id;
  req.topology_version = request.topology_version;
  req.parameter_version = request.parameter_version;
  req.packet_seq = request.packet_seq;
  req.inputs = n == 0 ? nullptr : views.data();
  req.input_ports = n == 0 ? nullptr : ports.data();
  req.input_count = static_cast<uint32_t>(n);
  req.parameter_json = parameters.c_str();
  req.completion_sink = sink->c_handle();
  req.batch_id = request.batch_id;
  req.batch_index = request.batch_index;
  req.batch_size = request.batch_size;
  const ge_status st = vtable_.submit(handle_, &req);
  return st.code == GE_STATUS_OK ? Status::Ok() : StatusFromC(st);
}

Status PluginOperator::Close(const CloseRequest& request) {
  ge_close_request req{};
  req.header.struct_size = sizeof(ge_close_request);
  req.header.abi_major = GE_ABI_MAJOR;
  req.session_id = request.session_id;
  req.topology_version = request.topology_version;
  req.fast_shutdown = request.fast_shutdown ? 1 : 0;
  closed_ = true;
  const ge_status st = vtable_.close(handle_, &req);
  return st.code == GE_STATUS_OK ? Status::Ok() : StatusFromC(st);
}

}  // namespace ge
