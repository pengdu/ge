#ifndef GE_C_GE_ABI_H_
#define GE_C_GE_ABI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define GE_EXPORT __declspec(dllexport)
#else
#define GE_EXPORT __attribute__((visibility("default")))
#endif

#define GE_ABI_MAJOR_NUMBER 1
#define GE_ABI_MAJOR 1u

typedef uint64_t ge_graph_id;
typedef uint64_t ge_session_id;
typedef uint64_t ge_node_id;
typedef uint64_t ge_edge_id;
typedef uint64_t ge_packet_seq;
typedef uint64_t ge_topology_version;
typedef uint64_t ge_parameter_version;
typedef uint64_t ge_operation_id;
typedef uint64_t ge_plugin_id;
typedef uint64_t ge_request_id;

typedef struct ge_struct_header {
  uint32_t struct_size;
  uint32_t abi_major;
} ge_struct_header;

#define GE_STRUCT_HEADER_INIT(type) \
  { (uint32_t)sizeof(type), GE_ABI_MAJOR }

typedef enum ge_status_code {
  GE_STATUS_OK = 0,
  GE_STATUS_INVALID_ARGUMENT,
  GE_STATUS_NOT_FOUND,
  GE_STATUS_ALREADY_EXISTS,
  GE_STATUS_VERSION_CONFLICT,
  GE_STATUS_GRAPH_INVALID,
  GE_STATUS_CAPABILITY_CONFLICT,
  GE_STATUS_SHARED_DEPENDENCY,
  GE_STATUS_RESOURCE_EXHAUSTED,
  GE_STATUS_NODE_WARMUP_FAILED,
  GE_STATUS_PARAMETER_UNSUPPORTED,
  GE_STATUS_PARAMETER_APPLY_FAILED,
  GE_STATUS_PLUGIN_ABI_MISMATCH,
  GE_STATUS_PLUGIN_MANIFEST_INVALID,
  GE_STATUS_PLUGIN_RETIRED,
  GE_STATUS_PLUGIN_PHYSICAL_UNLOAD_UNSAFE,
  GE_STATUS_WOULD_BLOCK,
  GE_STATUS_CANCELLED,
  GE_STATUS_INTERNAL
} ge_status_code;

typedef struct ge_status {
  ge_struct_header header;
  ge_status_code code;
  uint8_t retryable;
  const char* message;
  const char* detail_json;
} ge_status;

typedef struct ge_engine_t* ge_engine_handle;
typedef struct ge_session_t* ge_session_handle;
typedef struct ge_operator_t* ge_operator_handle;
typedef struct ge_buffer_t* ge_buffer_handle;
typedef struct ge_completion_sink_t* ge_completion_sink_handle;
typedef struct ge_subscription_t* ge_subscription_handle;

/* Releases memory wrapped through ge_host_api.buffer_wrap once the last
 * reference drops. */
typedef void (*ge_buffer_release_fn)(void* data, size_t size, void* context);

typedef enum ge_memory_kind {
  GE_MEMORY_HOST,
  GE_MEMORY_PINNED,
  GE_MEMORY_CUDA_DEVICE,
  GE_MEMORY_DMABUF
} ge_memory_kind;

typedef struct ge_buffer_view {
  ge_struct_header header;
  void* data;
  size_t size;
  ge_memory_kind memory_kind;
  int32_t device_id;
} ge_buffer_view;

#define GE_PACKET_FLAG_KEYFRAME 0x1u
#define GE_PACKET_FLAG_EOS 0x2u
#define GE_PACKET_FLAG_EVENT 0x4u
#define GE_PACKET_FLAG_DROPPED 0x8u
/* Tail-appended: first packet of a new output segment (a keyframe;
 * segmented encoders set it, segmenting muxers rotate their file on it). */
#define GE_PACKET_FLAG_SEGMENT_START 0x10u

typedef struct ge_packet_view {
  ge_struct_header header;
  ge_packet_seq seq;
  int64_t pts_ns;
  int64_t dts_ns;
  uint32_t flags;
  ge_topology_version topology_version;
  ge_parameter_version parameter_version;
  const char* type_tag;
  const char* metadata_json;
  ge_buffer_handle payload;
} ge_packet_view;

typedef enum ge_event_kind {
  GE_EVENT_DATA_PLANE,
  GE_EVENT_OBSERVATION
} ge_event_kind;

typedef enum ge_severity {
  GE_SEVERITY_DEBUG,
  GE_SEVERITY_INFO,
  GE_SEVERITY_WARNING,
  GE_SEVERITY_ERROR
} ge_severity;

typedef struct ge_event {
  ge_struct_header header;
  uint64_t event_id;
  ge_event_kind kind;
  const char* type;
  ge_severity severity;
  ge_session_id session_id;
  ge_node_id source_node_id;
  int64_t timestamp_ns;
  uint8_t has_seq;
  ge_packet_seq seq;
  uint8_t has_pts;
  int64_t pts_ns;
  const char* detail_json;
} ge_event;

typedef struct ge_log_record {
  ge_struct_header header;
  ge_severity severity;
  ge_session_id session_id;
  ge_node_id node_id;
  const char* message;
  const char* fields_json;
} ge_log_record;

#ifdef __cplusplus
}
#endif

#endif
