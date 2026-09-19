#ifndef GE_C_GE_PLUGIN_SDK_H_
#define GE_C_GE_PLUGIN_SDK_H_

/* Convenience macros for plugin authors (PLG-9). Header-only; a plugin
 * only needs ge_plugin.h + this file and must not link the engine. */

#include <stddef.h>

#include <ge/c/ge_plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GE_STRUCT_INIT(type) \
  { (uint32_t)sizeof(type), GE_ABI_MAJOR }

#define GE_OPERATOR_VTABLE_INIT(create_fn, open_fn, process_fn, submit_fn, \
                                close_fn, destroy_fn)                       \
  {                                                                        \
    GE_STRUCT_INIT(ge_operator_vtable), (create_fn), (open_fn),            \
        (process_fn), (submit_fn), (close_fn), (destroy_fn)                \
  }

#define GE_OPERATOR_DESCRIPTOR_INIT(type, version, cap_json, schema_json, \
                                    description, vtable_ptr, op_flags,   \
                                    parallelism, inference_ms)           \
  {                                                                      \
    GE_STRUCT_INIT(ge_operator_descriptor), (type), (version),           \
        (cap_json), (schema_json), (description), (vtable_ptr),          \
        (op_flags), (parallelism), (inference_ms)                        \
  }

#define GE_PLUGIN_DESCRIPTOR_INIT(id, manifest, ops, count)         \
  {                                                                 \
    GE_STRUCT_INIT(ge_plugin_descriptor), (id), GE_BUILD_FINGERPRINT, \
        (manifest), (ops), (count)                                  \
  }

static inline ge_status ge_status_ok(void) {
  ge_status s;
  s.header.struct_size = (uint32_t)sizeof(ge_status);
  s.header.abi_major = GE_ABI_MAJOR;
  s.code = GE_STATUS_OK;
  s.retryable = 0;
  s.message = 0;
  s.detail_json = 0;
  return s;
}

static inline ge_status ge_status_make(ge_status_code code, const char* message) {
  ge_status s = ge_status_ok();
  s.code = code;
  s.message = message;
  return s;
}

/* True when the host's ge_host_api is new enough to carry |member|. */
#define GE_HOST_API_HAS(api, member) \
  ((api)->header.struct_size >= offsetof(ge_host_api, member) + sizeof((api)->member))

static inline void ge_packet_view_init(ge_packet_view* v) {
  v->header.struct_size = (uint32_t)sizeof(ge_packet_view);
  v->header.abi_major = GE_ABI_MAJOR;
  v->seq = 0;
  v->pts_ns = 0;
  v->dts_ns = 0;
  v->flags = 0;
  v->topology_version = 0;
  v->parameter_version = 0;
  v->type_tag = 0;
  v->metadata_json = 0;
  v->payload = 0;
}

static inline ge_status ge_emit(const ge_host_api* api, void* callback_context,
                                const char* port, const ge_packet_view* packet) {
  ge_emit_args args;
  args.header.struct_size = (uint32_t)sizeof(ge_emit_args);
  args.header.abi_major = GE_ABI_MAJOR;
  args.callback_context = callback_context;
  args.output_port = port;
  args.packet = packet;
  return api->emit(&args);
}

/* True when the engine's ge_submit_request carries |member| (ABI 1 tail
 * additions such as batch_id/batch_index/batch_size). */
#define GE_SUBMIT_REQUEST_HAS(req, member) \
  ((req)->header.struct_size >= offsetof(ge_submit_request, member) + sizeof((req)->member))

static inline void ge_completion_event_init(ge_completion_event* ev,
                                            const ge_submit_request* req) {
  ev->header.struct_size = (uint32_t)sizeof(ge_completion_event);
  ev->header.abi_major = GE_ABI_MAJOR;
  ev->request_id = req->request_id;
  ev->session_id = req->session_id;
  ev->topology_version = req->topology_version;
  ev->parameter_version = req->parameter_version;
  ev->packet_seq = req->packet_seq;
  ev->status = ge_status_ok();
  ev->outputs = 0;
  ev->output_ports = 0;
  ev->output_count = 0;
}

#ifdef __cplusplus
}
#endif

#endif
