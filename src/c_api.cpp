#include <ge/c/ge_engine.h>
#include <ge/cpp/engine.h>
#include <ge/cpp/graph_spec_json.h>
#include <ge/cpp/plugin_operator.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// Opaque handles (13 §3.2)
// ---------------------------------------------------------------------------

struct ge_engine_t {
  std::unique_ptr<ge::Engine> engine;
};

struct ge_session_t {
  ge_engine_t* owner = nullptr;
  ge::SessionId id = 0;
};

struct ge_subscription_t {
  ge_engine_t* owner = nullptr;
  ge::SubscriptionId id = 0;
};

namespace {

// ge_status text lives in thread-local storage until the next call on the
// same thread (13 §3.1 "message 与 detail_json 在调用返回前有效").
ge_status ToC(const ge::Status& s) noexcept {
  thread_local ge::CStatusStorage storage;
  ge_status out;
  ge::StatusToC(s, &storage, &out);
  return out;
}

ge_status Invalid(const char* what) noexcept {
  return ToC(ge::Status::InvalidArgument(what));
}

char* Dup(const std::string& s) { return ::strdup(s.c_str()); }

ge::CallerContext ParseCaller(const char* json, ge::Status* error) {
  ge::CallerContext c;
  if (json == nullptr || json[0] == '\0') return c;
  ge::JsonParseResult parsed = ge::ParseJson(json);
  if (!parsed.ok() || !parsed.value->is_object()) {
    *error = ge::Status::InvalidArgument("caller_context_json must be a JSON object");
    return c;
  }
  if (const auto v = parsed.value->GetString("caller_id")) c.caller_id = *v;
  if (const auto v = parsed.value->GetString("request_id")) c.request_id = *v;
  return c;
}

ge::Session* Resolve(ge_session_handle session) {
  if (session == nullptr || session->owner == nullptr) return nullptr;
  return session->owner->engine->FindSession(session->id);
}

template <typename F>
ge_status Guarded(F&& f) noexcept {
  try {
    return f();
  } catch (const std::exception& e) {
    return ToC(ge::Status::Internal(std::string("exception: ") + e.what()));
  } catch (...) {
    return ToC(ge::Status::Internal("unknown exception"));
  }
}

}  // namespace

extern "C" {

// ---------------------------------------------------------------------------
// Engine (13 §5.1)
// ---------------------------------------------------------------------------

GE_EXPORT ge_status ge_engine_create(const ge_engine_config* config, ge_engine_handle* out_engine) {
  return Guarded([&]() -> ge_status {
    if (out_engine == nullptr) return Invalid("out_engine is null");
    *out_engine = nullptr;
    if (config == nullptr || config->header.abi_major != GE_ABI_MAJOR ||
        config->header.struct_size < sizeof(ge_engine_config)) {
      return ToC(ge::Status::PluginAbiMismatch("ge_engine_config header mismatch"));
    }
    auto cfg = ge::EngineConfig::FromJson(config->plugin_search_paths_json, config->resource_limits_json,
                                          config->observability_config_json);
    if (!cfg.ok()) return ToC(cfg.status());
    auto engine = ge::Engine::Create(std::move(*cfg));
    if (!engine.ok()) return ToC(engine.status());
    auto* h = new ge_engine_t;
    h->engine = std::move(*engine);
    *out_engine = h;
    return ge::OkStatus();
  });
}

GE_EXPORT void ge_engine_destroy(ge_engine_handle engine) {
  if (engine == nullptr) return;
  delete engine;
}

GE_EXPORT ge_status ge_engine_load_plugin(ge_engine_handle engine, const char* manifest_path,
                                          ge_plugin_id* out_plugin_id) {
  return Guarded([&]() -> ge_status {
    if (engine == nullptr) return Invalid("engine is null");
    if (manifest_path == nullptr || out_plugin_id == nullptr) return Invalid("manifest_path/out_plugin_id is null");
    auto info = engine->engine->LoadPlugin(manifest_path);
    if (!info.ok()) return ToC(info.status());
    *out_plugin_id = info->id;
    return ge::OkStatus();
  });
}

GE_EXPORT ge_status ge_engine_retire_plugin(ge_engine_handle engine, ge_plugin_id plugin_id,
                                            uint8_t request_physical_unload,
                                            ge_operation_id* out_operation_id) {
  return Guarded([&]() -> ge_status {
    if (engine == nullptr) return Invalid("engine is null");
    if (out_operation_id == nullptr) return Invalid("out_operation_id is null");
    auto op = engine->engine->RetirePlugin(plugin_id, {.request_physical_unload = request_physical_unload != 0});
    if (!op.ok()) return ToC(op.status());
    *out_operation_id = *op;
    return ge::OkStatus();
  });
}

GE_EXPORT ge_status ge_engine_upgrade_operator(ge_engine_handle engine, const char* old_operator_key,
                                               const char* new_operator_key,
                                               ge_operation_id* out_operation_id) {
  return Guarded([&]() -> ge_status {
    if (engine == nullptr) return Invalid("engine is null");
    if (old_operator_key == nullptr || new_operator_key == nullptr || out_operation_id == nullptr) {
      return Invalid("operator keys/out_operation_id are null");
    }
    const auto old_key = ge::OperatorKey::Parse(old_operator_key);
    const auto new_key = ge::OperatorKey::Parse(new_operator_key);
    if (!old_key || !new_key) return Invalid("operator key must be type@version");
    auto report = engine->engine->UpgradeOperator(*old_key, *new_key);
    if (!report.ok()) return ToC(report.status());
    *out_operation_id = report->operation;
    if (!report->all_succeeded()) {
      return ToC(ge::Status(GE_STATUS_INTERNAL, "some sessions failed to upgrade", false,
                            report->ToJson().Serialize()));
    }
    return ge::OkStatus();
  });
}

// ---------------------------------------------------------------------------
// Session (13 §5.2)
// ---------------------------------------------------------------------------

GE_EXPORT ge_status ge_session_create(ge_engine_handle engine, const char* graph_spec_json,
                                      const char* caller_context_json, ge_session_handle* out_session,
                                      ge_operation_id* out_operation_id) {
  return Guarded([&]() -> ge_status {
    if (engine == nullptr) return Invalid("engine is null");
    if (graph_spec_json == nullptr || out_session == nullptr) return Invalid("graph_spec_json/out_session is null");
    *out_session = nullptr;
    auto spec = ge::GraphSpecParser::ParseGraph(graph_spec_json);
    if (!spec.ok()) return ToC(spec.status());
    ge::Status err;
    ge::CallerContext caller = ParseCaller(caller_context_json, &err);
    if (!err.ok()) return ToC(err);
    ge::OperationId op = 0;
    auto s = engine->engine->CreateSession(*spec, std::move(caller), &op);
    if (out_operation_id != nullptr) *out_operation_id = op;
    if (!s.ok()) return ToC(s.status());
    auto* h = new ge_session_t;
    h->owner = engine;
    h->id = (*s)->id();
    *out_session = h;
    return ge::OkStatus();
  });
}

GE_EXPORT ge_status ge_session_start(ge_session_handle session) {
  return Guarded([&]() -> ge_status {
    ge::Session* s = Resolve(session);
    if (s == nullptr) return Invalid("session is null or destroyed");
    return ToC(s->Start());
  });
}

GE_EXPORT ge_status ge_session_pause(ge_session_handle session) {
  return Guarded([&]() -> ge_status {
    ge::Session* s = Resolve(session);
    if (s == nullptr) return Invalid("session is null or destroyed");
    return ToC(s->Pause());
  });
}

GE_EXPORT ge_status ge_session_resume(ge_session_handle session) {
  return Guarded([&]() -> ge_status {
    ge::Session* s = Resolve(session);
    if (s == nullptr) return Invalid("session is null or destroyed");
    return ToC(s->Resume());
  });
}

GE_EXPORT ge_status ge_session_stop(ge_session_handle session, uint8_t fast_shutdown,
                                    ge_operation_id* out_operation_id) {
  return Guarded([&]() -> ge_status {
    ge::Session* s = Resolve(session);
    if (s == nullptr) return Invalid("session is null or destroyed");
    if (out_operation_id == nullptr) return Invalid("out_operation_id is null");
    auto op = s->Stop(fast_shutdown != 0);
    if (!op.ok()) return ToC(op.status());
    *out_operation_id = *op;
    return ge::OkStatus();
  });
}

GE_EXPORT void ge_session_destroy(ge_session_handle session) {
  if (session == nullptr) return;
  if (session->owner != nullptr) (void)session->owner->engine->DestroySession(session->id);
  delete session;
}

GE_EXPORT ge_status ge_session_get_snapshot_json(ge_session_handle session, char** out_json) {
  return Guarded([&]() -> ge_status {
    ge::Session* s = Resolve(session);
    if (s == nullptr) return Invalid("session is null or destroyed");
    if (out_json == nullptr) return Invalid("out_json is null");
    *out_json = Dup(s->Snapshot().Serialize());
    if (*out_json == nullptr) return ToC(ge::Status::ResourceExhausted("out of memory"));
    return ge::OkStatus();
  });
}

GE_EXPORT void ge_string_free(ge_engine_handle, char* str) {
  std::free(str);
}

// ---------------------------------------------------------------------------
// Mutation / parameters (13 §5.3)
// ---------------------------------------------------------------------------

GE_EXPORT ge_status ge_session_dry_run_patch(ge_session_handle session, const char* patch_json,
                                             char** out_result_json) {
  return Guarded([&]() -> ge_status {
    ge::Session* s = Resolve(session);
    if (s == nullptr) return Invalid("session is null or destroyed");
    if (patch_json == nullptr || out_result_json == nullptr) return Invalid("patch_json/out_result_json is null");
    auto patch = ge::GraphSpecParser::ParsePatch(patch_json);
    if (!patch.ok()) return ToC(patch.status());
    auto r = s->DryRun(*patch);
    if (!r.ok()) return ToC(r.status());
    *out_result_json = Dup(r->ToJson().Serialize());
    return ge::OkStatus();
  });
}

GE_EXPORT ge_status ge_session_apply_patch(ge_session_handle session, const char* patch_json,
                                           const char* caller_context_json,
                                           ge_operation_id* out_operation_id) {
  return Guarded([&]() -> ge_status {
    ge::Session* s = Resolve(session);
    if (s == nullptr) return Invalid("session is null or destroyed");
    if (patch_json == nullptr || out_operation_id == nullptr) return Invalid("patch_json/out_operation_id is null");
    auto patch = ge::GraphSpecParser::ParsePatch(patch_json);
    if (!patch.ok()) return ToC(patch.status());
    ge::Status err;
    ge::CallerContext caller = ParseCaller(caller_context_json, &err);
    if (!err.ok()) return ToC(err);
    auto op = s->Apply(std::move(*patch), std::move(caller));
    if (!op.ok()) return ToC(op.status());
    *out_operation_id = *op;
    return ge::OkStatus();
  });
}

GE_EXPORT ge_status ge_session_set_node_parameters(ge_session_handle session, ge_node_id node_id,
                                                   const char* parameters_json,
                                                   const char* caller_context_json,
                                                   ge_parameter_version* out_version,
                                                   ge_operation_id* out_operation_id) {
  return Guarded([&]() -> ge_status {
    ge::Session* s = Resolve(session);
    if (s == nullptr) return Invalid("session is null or destroyed");
    if (parameters_json == nullptr) return Invalid("parameters_json is null");
    ge::JsonParseResult parsed = ge::ParseJson(parameters_json);
    if (!parsed.ok()) return ToC(ge::Status::InvalidArgument("parameters_json: " + parsed.error));
    ge::Status err;
    ge::CallerContext caller = ParseCaller(caller_context_json, &err);
    if (!err.ok()) return ToC(err);
    const ge::NodeRuntime* node = s->current_topology()->FindNode(node_id);
    if (node == nullptr) return ToC(ge::Status::NotFound("node " + std::to_string(node_id) + " not found"));
    auto r = s->SetParameters(node->external_id(), std::move(*parsed.value), std::move(caller));
    if (!r.ok()) return ToC(r.status());
    if (out_version != nullptr) *out_version = r->version;
    if (out_operation_id != nullptr) *out_operation_id = r->operation;
    return ge::OkStatus();
  });
}

// ---------------------------------------------------------------------------
// Events / operations / capability (13 §5.4)
// ---------------------------------------------------------------------------

GE_EXPORT ge_status ge_session_subscribe_events(ge_session_handle session, const ge_event_filter* filter,
                                                ge_event_callback callback, void* user_data,
                                                ge_subscription_handle* out_subscription) {
  return Guarded([&]() -> ge_status {
    ge::Session* s = Resolve(session);
    if (s == nullptr) return Invalid("session is null or destroyed");
    if (callback == nullptr || out_subscription == nullptr) return Invalid("callback/out_subscription is null");
    ge::EventFilter f;
    if (filter != nullptr) {
      if (filter->header.abi_major != GE_ABI_MAJOR || filter->header.struct_size < sizeof(ge_event_filter)) {
        return ToC(ge::Status::PluginAbiMismatch("ge_event_filter header mismatch"));
      }
      auto parsed = ge::EventFilter::ParseJson(filter->filter_json != nullptr ? filter->filter_json : "");
      if (!parsed.ok()) return ToC(parsed.status());
      f = std::move(*parsed);
    }
    f.session = s->id();  // a session subscription only sees its own events
    const ge::SubscriptionId id = session->owner->engine->events().Subscribe(
        std::move(f), [callback, user_data](const ge::Event& e) {
          const std::string detail = e.detail.Serialize();
          ge_event ev{};
          ev.header.struct_size = sizeof(ge_event);
          ev.header.abi_major = GE_ABI_MAJOR;
          ev.event_id = e.event_id;
          ev.kind = e.kind == ge::EventKind::kDataPlane ? GE_EVENT_DATA_PLANE : GE_EVENT_OBSERVATION;
          ev.type = e.type.c_str();
          ev.severity = static_cast<ge_severity>(e.severity);
          ev.session_id = e.session;
          ev.source_node_id = e.source_node;
          ev.timestamp_ns = e.timestamp_ns;
          ev.has_seq = e.seq ? 1 : 0;
          ev.seq = e.seq.value_or(0);
          ev.has_pts = e.pts_ns ? 1 : 0;
          ev.pts_ns = e.pts_ns.value_or(0);
          ev.detail_json = detail.c_str();
          callback(&ev, user_data);
        });
    auto* h = new ge_subscription_t;
    h->owner = session->owner;
    h->id = id;
    *out_subscription = h;
    return ge::OkStatus();
  });
}

GE_EXPORT void ge_subscription_cancel(ge_subscription_handle subscription) {
  if (subscription == nullptr) return;
  if (subscription->owner != nullptr) subscription->owner->engine->events().Cancel(subscription->id);
  delete subscription;
}

GE_EXPORT ge_status ge_engine_get_operation_json(ge_engine_handle engine, ge_operation_id operation,
                                                 char** out_json) {
  return Guarded([&]() -> ge_status {
    if (engine == nullptr) return Invalid("engine is null");
    if (out_json == nullptr) return Invalid("out_json is null");
    const auto rec = engine->engine->operations().Get(operation);
    if (!rec) return ToC(ge::Status::NotFound("operation " + std::to_string(operation) + " not found"));
    *out_json = Dup(rec->ToJson().Serialize());
    return ge::OkStatus();
  });
}

GE_EXPORT ge_status ge_engine_query_audit_json(ge_engine_handle engine, const char* filter_json,
                                               char** out_json) {
  return Guarded([&]() -> ge_status {
    if (engine == nullptr) return Invalid("engine is null");
    if (out_json == nullptr) return Invalid("out_json is null");
    ge::AuditFilter filter;
    if (filter_json != nullptr && filter_json[0] != '\0') {
      const auto parsed = ge::ParseJson(filter_json);
      if (!parsed.ok()) return ToC(ge::Status::InvalidArgument("filter_json: " + parsed.error));
      auto f = ge::AuditFilter::FromJson(*parsed.value);
      if (!f.ok()) return ToC(f.status());
      filter = *f;
    }
    *out_json = Dup(engine->engine->audit().QueryJson(filter).Serialize());
    return ge::OkStatus();
  });
}

GE_EXPORT ge_status ge_engine_render_prometheus(ge_engine_handle engine, char** out_text) {
  return Guarded([&]() -> ge_status {
    if (engine == nullptr) return Invalid("engine is null");
    if (out_text == nullptr) return Invalid("out_text is null");
    *out_text = Dup(engine->engine->RenderPrometheus());
    return ge::OkStatus();
  });
}

GE_EXPORT ge_status ge_engine_get_capability_json(ge_engine_handle engine, const char* operator_key,
                                                  char** out_json) {
  return Guarded([&]() -> ge_status {
    if (engine == nullptr) return Invalid("engine is null");
    if (operator_key == nullptr || out_json == nullptr) return Invalid("operator_key/out_json is null");
    const auto key = ge::OperatorKey::Parse(operator_key);
    if (!key) return Invalid("operator key must be type@version");
    auto cap = engine->engine->GetCapability(*key);
    if (!cap.ok()) return ToC(cap.status());
    *out_json = Dup(cap->Serialize());
    return ge::OkStatus();
  });
}

}  // extern "C"
