#ifndef GE_C_GE_PLUGIN_H_
#define GE_C_GE_PLUGIN_H_

#include <ge/c/ge_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ge_operator_vtable;
struct ge_operator_create_args;

typedef struct ge_operator_descriptor {
  ge_struct_header header;
  const char* type_name;
  const char* semantic_version;
  const char* capability_json;
  const char* parameter_schema_json;
  const char* functional_description;
  const struct ge_operator_vtable* vtable;
  uint32_t flags;
  uint32_t max_parallelism;
  uint64_t max_inference_ms;
} ge_operator_descriptor;

#define GE_OPERATOR_FLAG_STATEFUL 0x1u
#define GE_OPERATOR_FLAG_ASYNC 0x2u
#define GE_OPERATOR_FLAG_HOT_UPDATE 0x4u
#define GE_OPERATOR_FLAG_NO_OWNED_THREADS 0x8u

typedef struct ge_plugin_descriptor {
  ge_struct_header header;
  const char* plugin_id;
  const char* build_fingerprint;
  const char* manifest_json;
  const ge_operator_descriptor* operators;
  uint32_t operator_count;
} ge_plugin_descriptor;

typedef const ge_plugin_descriptor* (*ge_plugin_get_descriptor_fn)(
    uint32_t requested_abi_major);

typedef ge_status (*ge_plugin_create_operator_fn)(
    const struct ge_operator_create_args* args,
    ge_operator_handle* out_operator);

typedef struct ge_emit_args {
  ge_struct_header header;
  void* callback_context;
  const char* output_port;
  const ge_packet_view* packet;
} ge_emit_args;

typedef struct ge_completion_event {
  ge_struct_header header;
  ge_request_id request_id;
  ge_session_id session_id;
  ge_topology_version topology_version;
  ge_parameter_version parameter_version;
  ge_packet_seq packet_seq;
  ge_status status;
  const ge_packet_view* outputs;
  const char* const* output_ports;
  uint32_t output_count;
} ge_completion_event;

typedef struct ge_host_api {
  ge_struct_header header;
  ge_status (*buffer_retain)(ge_buffer_handle buffer);
  void (*buffer_release)(ge_buffer_handle buffer);
  ge_status (*buffer_get_view)(ge_buffer_handle buffer,
                                ge_buffer_view* out_view);
  ge_status (*emit)(const ge_emit_args* args);
  ge_status (*completion_push)(ge_completion_sink_handle sink,
                               const ge_completion_event* event);
  ge_status (*event_publish)(const ge_event* event);
  void (*log)(const ge_log_record* record);
  /* ABI 1 tail additions (12 s3.1: same-major append only). A plugin built
   * against an older header sees a smaller struct_size and must not touch
   * these members. */
  ge_status (*buffer_alloc)(void* host_context, size_t size,
                            ge_memory_kind memory_kind, int32_t device_id,
                            ge_buffer_handle* out_buffer);
  ge_status (*buffer_wrap)(void* host_context, const ge_buffer_view* view,
                           ge_buffer_release_fn release, void* release_context,
                           ge_buffer_handle* out_buffer);
} ge_host_api;

/* Toolchain fingerprint (PLG-3). The engine and every plugin evaluate this
 * macro at compile time; the registry rejects a plugin whose descriptor or
 * manifest fingerprint differs from the engine's own. */
#if defined(__clang__)
#define GE_FP_COMPILER_ "clang-" GE_STR_(__clang_major__) "." GE_STR_(__clang_minor__)
#elif defined(__GNUC__)
#define GE_FP_COMPILER_ "gcc-" GE_STR_(__GNUC__) "." GE_STR_(__GNUC_MINOR__)
#elif defined(_MSC_VER)
#define GE_FP_COMPILER_ "msvc-" GE_STR_(_MSC_VER)
#else
#define GE_FP_COMPILER_ "unknown"
#endif
/* The C++ runtime segment must be identical in C and C++ translation
 * units, so it is derived from the platform (a C plugin still links
 * against the same runtime the engine uses); C++ TUs verify it. */
#if defined(_MSC_VER)
#define GE_FP_STDLIB_ "msvcrt"
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__ANDROID__)
#define GE_FP_STDLIB_ "libc++"
#else
#define GE_FP_STDLIB_ "libstdc++"
#endif
#if defined(__cplusplus)
#if defined(_LIBCPP_VERSION) && !(defined(__APPLE__) || defined(__FreeBSD__) || defined(__ANDROID__))
#error "GE_BUILD_FINGERPRINT assumes libstdc++ on this platform but libc++ is in use; adjust GE_FP_STDLIB_"
#endif
#if defined(__GLIBCXX__) && (defined(__APPLE__) || defined(__FreeBSD__) || defined(__ANDROID__))
#error "GE_BUILD_FINGERPRINT assumes libc++ on this platform but libstdc++ is in use; adjust GE_FP_STDLIB_"
#endif
#endif
#if defined(__APPLE__)
#define GE_FP_OS_ "darwin"
#elif defined(__linux__)
#define GE_FP_OS_ "linux"
#elif defined(_WIN32)
#define GE_FP_OS_ "windows"
#else
#define GE_FP_OS_ "unknown"
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#define GE_FP_ARCH_ "arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#define GE_FP_ARCH_ "x86_64"
#else
#define GE_FP_ARCH_ "unknown"
#endif
#define GE_STR2_(x) #x
#define GE_STR_(x) GE_STR2_(x)
#define GE_BUILD_FINGERPRINT \
  GE_FP_COMPILER_ "-" GE_FP_STDLIB_ "-" GE_FP_OS_ "-" GE_FP_ARCH_ "-abi" GE_STR_(GE_ABI_MAJOR_NUMBER)

typedef struct ge_operator_create_args {
  ge_struct_header header;
  const char* operator_key;
  ge_node_id node_id;
  const char* options_json;
  const ge_host_api* host_api;
  void* host_context;
} ge_operator_create_args;

typedef struct ge_open_request {
  ge_struct_header header;
  ge_session_id session_id;
  ge_topology_version topology_version;
  const char* options_json;
} ge_open_request;

typedef struct ge_close_request {
  ge_struct_header header;
  ge_session_id session_id;
  ge_topology_version topology_version;
  uint8_t fast_shutdown;
} ge_close_request;

#define GE_PROCESS_FLAG_FLUSH 0x1u

typedef struct ge_process_request {
  ge_struct_header header;
  ge_session_id session_id;
  ge_topology_version topology_version;
  ge_parameter_version parameter_version;
  uint32_t flags;
  const ge_packet_view* inputs;
  const char* const* input_ports;
  uint32_t input_count;
  const char* parameter_json;
  void* callback_context;
} ge_process_request;

typedef struct ge_submit_request {
  ge_struct_header header;
  ge_request_id request_id;
  ge_session_id session_id;
  ge_topology_version topology_version;
  ge_parameter_version parameter_version;
  ge_packet_seq packet_seq;
  const ge_packet_view* inputs;
  const char* const* input_ports;
  uint32_t input_count;
  const char* parameter_json;
  ge_completion_sink_handle completion_sink;
  /* ABI 1 tail additions (12 s3.1). Batch framing (ASY-5/6): the engine
   * calls submit for every member of one batch back to back on the same
   * thread, never interleaved with another batch of the same operator
   * instance. batch_index runs 0..batch_size-1; the plugin may defer the
   * backend call until batch_index == batch_size - 1 and must then push one
   * completion per request_id. Unbatched requests have batch_size == 1. */
  uint64_t batch_id;
  uint32_t batch_index;
  uint32_t batch_size;
} ge_submit_request;

typedef ge_status (*ge_operator_open_fn)(ge_operator_handle op,
                                         const ge_open_request* request);
typedef ge_status (*ge_operator_process_fn)(
    ge_operator_handle op, const ge_process_request* request);
typedef ge_status (*ge_operator_submit_fn)(ge_operator_handle op,
                                           const ge_submit_request* request);
typedef ge_status (*ge_operator_close_fn)(ge_operator_handle op,
                                          const ge_close_request* request);
typedef void (*ge_operator_destroy_fn)(ge_operator_handle op);

typedef struct ge_operator_vtable {
  ge_struct_header header;
  ge_plugin_create_operator_fn create;
  ge_operator_open_fn open;
  ge_operator_process_fn process;
  ge_operator_submit_fn submit;
  ge_operator_close_fn close;
  ge_operator_destroy_fn destroy;
} ge_operator_vtable;

#ifdef __cplusplus
}
#endif

#endif
