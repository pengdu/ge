/* Sample plugin (15 P4 task 8): three sync operators written in C11.
 *   Src@1.0.0  source, emits `count` Bytes packets then EOS
 *   Pass@1.0.0 forwards "in" to "out", applies the hot parameter "gain"
 *              by writing it into the first byte, migratable
 *   Sink@1.0.0 counts packets; reports through log()
 * plus one async operator (15 P5, fake backend without threads, ASY-2):
 *   AsyncPass@1.0.0 buffers submits of one batch and pushes their
 *              completions in reverse order when the last member arrives
 *              (exercises the ReorderBuffer). Parameters: "fail_at" makes
 *              request N complete with an error, "drop_at" never completes
 *              request N (timeout gap), "gain" is stamped like Pass.
 * A second build of this file with GE_SAMPLE_VERSION_2 registers the same
 * operators as @2.0.0 (upgrade tests). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ge/c/ge_plugin_sdk.h>

#ifdef GE_SAMPLE_VERSION_2
#define VER "2.0.0"
#define PLUGIN_ID "com.example.sample2"
#else
#define VER "1.0.0"
#define PLUGIN_ID "com.example.sample"
#endif

/* ------------------------------------------------------------------------- */

#define ASYNC_MAX_BATCH 64

typedef struct async_member {
  ge_completion_event event;
  ge_packet_view out;
  const char* port;
  int drop;
  ge_buffer_handle payload; /* retained for the completion */
} async_member;

typedef struct sample_op {
  const ge_host_api* api;
  void* host_context;
  ge_node_id node;
  int kind; /* 0 src, 1 pass, 2 sink, 3 async pass */
  long count;
  long next;
  long seen;
  int opened;
  int closed;
  int fast_close;
  /* async pass */
  ge_completion_sink_handle sink;
  async_member batch[ASYNC_MAX_BATCH];
  uint32_t batch_len;
  uint64_t batch_id;
} sample_op;

static long json_int(const char* json, const char* key, long fallback) {
  if (json == 0) return fallback;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  const char* p = strstr(json, pattern);
  if (p == 0) return fallback;
  return strtol(p + strlen(pattern), 0, 10);
}

static ge_status sample_create(const ge_operator_create_args* args, ge_operator_handle* out,
                               int kind) {
  if (args == 0 || out == 0 || args->host_api == 0) {
    return ge_status_make(GE_STATUS_INVALID_ARGUMENT, "bad create args");
  }
  if (args->header.abi_major != GE_ABI_MAJOR) {
    return ge_status_make(GE_STATUS_PLUGIN_ABI_MISMATCH, "abi major");
  }
  sample_op* op = (sample_op*)calloc(1, sizeof(sample_op));
  if (op == 0) return ge_status_make(GE_STATUS_RESOURCE_EXHAUSTED, "oom");
  op->api = args->host_api;
  op->host_context = args->host_context;
  op->node = args->node_id;
  op->kind = kind;
  op->count = json_int(args->options_json, "count", 10);
  if (json_int(args->options_json, "fail_create", 0) != 0) {
    free(op);
    return ge_status_make(GE_STATUS_INTERNAL, "fail_create requested");
  }
  *out = (ge_operator_handle)op;
  return ge_status_ok();
}

static ge_status src_create(const ge_operator_create_args* a, ge_operator_handle* o) {
  return sample_create(a, o, 0);
}
static ge_status pass_create(const ge_operator_create_args* a, ge_operator_handle* o) {
  return sample_create(a, o, 1);
}
static ge_status sink_create(const ge_operator_create_args* a, ge_operator_handle* o) {
  return sample_create(a, o, 2);
}
static ge_status async_create(const ge_operator_create_args* a, ge_operator_handle* o) {
  return sample_create(a, o, 3);
}

static ge_status sample_open(ge_operator_handle h, const ge_open_request* req) {
  sample_op* op = (sample_op*)h;
  if (json_int(req->options_json, "fail_open", 0) != 0) {
    return ge_status_make(GE_STATUS_RESOURCE_EXHAUSTED, "fail_open requested");
  }
  op->opened = 1;
  return ge_status_ok();
}

static ge_status src_process(sample_op* op, const ge_process_request* req) {
  if (req->flags & GE_PROCESS_FLAG_FLUSH) return ge_status_ok();
  ge_packet_view pkt;
  ge_packet_view_init(&pkt);
  if (op->next >= op->count) {
    pkt.flags = GE_PACKET_FLAG_EOS; /* 13 §4.5: exhausted */
    return ge_emit(op->api, req->callback_context, "out", &pkt);
  }
  if (!GE_HOST_API_HAS(op->api, buffer_alloc)) {
    return ge_status_make(GE_STATUS_PLUGIN_ABI_MISMATCH, "host has no buffer_alloc");
  }
  ge_buffer_handle buf = 0;
  ge_status st = op->api->buffer_alloc(op->host_context, 8, GE_MEMORY_HOST, -1, &buf);
  if (st.code != GE_STATUS_OK) return st;
  ge_buffer_view view;
  view.header.struct_size = (uint32_t)sizeof(ge_buffer_view);
  view.header.abi_major = GE_ABI_MAJOR;
  st = op->api->buffer_get_view(buf, &view);
  if (st.code != GE_STATUS_OK) {
    op->api->buffer_release(buf);
    return st;
  }
  long seq = op->next + 1;
  memcpy(view.data, &seq, sizeof(seq));
  pkt.seq = (ge_packet_seq)seq;
  pkt.pts_ns = seq;
  pkt.type_tag = "Bytes";
  pkt.payload = buf;
  st = ge_emit(op->api, req->callback_context, "out", &pkt);
  op->api->buffer_release(buf); /* engine retained in emit */
  if (st.code == GE_STATUS_WOULD_BLOCK) return ge_status_ok(); /* retry same seq */
  if (st.code != GE_STATUS_OK) return st;
  op->next = seq;
  return ge_status_ok();
}

static ge_status pass_process(sample_op* op, const ge_process_request* req) {
  if (req->flags & GE_PROCESS_FLAG_FLUSH) return ge_status_ok();
  long gain = json_int(req->parameter_json, "gain", 0);
  long fail_at = json_int(req->parameter_json, "fail_at", 0);
  for (uint32_t i = 0; i < req->input_count; ++i) {
    const ge_packet_view* in = &req->inputs[i];
    if (in->flags & GE_PACKET_FLAG_EVENT) continue;
    ++op->seen;
    if (fail_at != 0 && op->seen == fail_at) {
      return ge_status_make(GE_STATUS_INTERNAL, "fail_at reached");
    }
    ge_packet_view out = *in;
    if (gain != 0 && in->payload != 0) {
      ge_buffer_view view;
      view.header.struct_size = (uint32_t)sizeof(ge_buffer_view);
      view.header.abi_major = GE_ABI_MAJOR;
      if (op->api->buffer_get_view(in->payload, &view).code == GE_STATUS_OK && view.size >= 8) {
        /* copy: never mutate a shared input payload */
        ge_buffer_handle nb = 0;
        ge_status st = op->api->buffer_alloc(op->host_context, view.size, GE_MEMORY_HOST, -1, &nb);
        if (st.code != GE_STATUS_OK) return st;
        ge_buffer_view nv = view;
        op->api->buffer_get_view(nb, &nv);
        memcpy(nv.data, view.data, view.size);
        ((unsigned char*)nv.data)[7] = (unsigned char)gain;
        out.payload = nb;
        st = ge_emit(op->api, req->callback_context, "out", &out);
        op->api->buffer_release(nb);
        if (st.code != GE_STATUS_OK && st.code != GE_STATUS_WOULD_BLOCK) return st;
        continue;
      }
    }
    ge_status st = ge_emit(op->api, req->callback_context, "out", &out);
    if (st.code != GE_STATUS_OK && st.code != GE_STATUS_WOULD_BLOCK) return st;
  }
  return ge_status_ok();
}

static ge_status sink_process(sample_op* op, const ge_process_request* req) {
  if (req->flags & GE_PROCESS_FLAG_FLUSH) {
    char fields[64];
    snprintf(fields, sizeof(fields), "{\"packets\":%ld}", op->seen);
    ge_log_record rec;
    rec.header.struct_size = (uint32_t)sizeof(ge_log_record);
    rec.header.abi_major = GE_ABI_MAJOR;
    rec.severity = GE_SEVERITY_INFO;
    rec.session_id = req->session_id;
    rec.node_id = op->node;
    rec.message = "sink flushed";
    rec.fields_json = fields;
    op->api->log(&rec);
    return ge_status_ok();
  }
  for (uint32_t i = 0; i < req->input_count; ++i) {
    if (req->inputs[i].flags & GE_PACKET_FLAG_EVENT) continue;
    ++op->seen;
    if (req->inputs[i].payload != 0) {
      ge_buffer_view view;
      view.header.struct_size = (uint32_t)sizeof(ge_buffer_view);
      view.header.abi_major = GE_ABI_MAJOR;
      if (op->api->buffer_get_view(req->inputs[i].payload, &view).code == GE_STATUS_OK &&
          view.size >= 8) {
        long seq = 0;
        memcpy(&seq, view.data, sizeof(long) < 8 ? sizeof(long) : 8);
        char detail[128];
        snprintf(detail, sizeof(detail), "{\"seq\":%ld,\"gain\":%d}", seq & 0xffffffffffL,
                 ((unsigned char*)view.data)[7]);
        ge_event ev;
        ev.header.struct_size = (uint32_t)sizeof(ge_event);
        ev.header.abi_major = GE_ABI_MAJOR;
        ev.event_id = 0;
        ev.kind = GE_EVENT_OBSERVATION;
        ev.type = "sample.packet";
        ev.severity = GE_SEVERITY_DEBUG;
        ev.session_id = req->session_id;
        ev.source_node_id = op->node;
        ev.timestamp_ns = 0;
        ev.has_seq = 1;
        ev.seq = req->inputs[i].seq;
        ev.has_pts = 1;
        ev.pts_ns = req->inputs[i].pts_ns;
        ev.detail_json = detail;
        op->api->event_publish(&ev);
      }
    }
  }
  return ge_status_ok();
}

/* Fake async backend: pushes every buffered completion of the current batch
 * (reverse order) through completion_push. Never blocks, never threads. */
static void async_flush(sample_op* op) {
  for (uint32_t i = op->batch_len; i > 0; --i) {
    async_member* m = &op->batch[i - 1];
    if (m->drop) {
      if (m->payload != 0) op->api->buffer_release(m->payload);
      continue;
    }
    m->event.outputs = &m->out;
    m->event.output_ports = &m->port;
    m->event.output_count = m->event.status.code == GE_STATUS_OK ? 1 : 0;
    /* One reference per output payload travels with the event (13 s4.6). */
    ge_status st = op->api->completion_push(op->sink, &m->event);
    if (st.code != GE_STATUS_OK && m->payload != 0) op->api->buffer_release(m->payload);
  }
  op->batch_len = 0;
}

static ge_status async_submit(ge_operator_handle h, const ge_submit_request* req) {
  sample_op* op = (sample_op*)h;
  if (!op->opened) return ge_status_make(GE_STATUS_INTERNAL, "submit before open");
  if (req->completion_sink == 0) return ge_status_make(GE_STATUS_INVALID_ARGUMENT, "no sink");
  op->sink = req->completion_sink;
  uint64_t batch_id = 0;
  uint32_t batch_index = 0;
  uint32_t batch_size = 1;
  if (GE_SUBMIT_REQUEST_HAS(req, batch_size)) {
    batch_id = req->batch_id;
    batch_index = req->batch_index;
    batch_size = req->batch_size;
  }
  if (batch_index == 0) {
    if (op->batch_len != 0) async_flush(op); /* defensive: unfinished batch */
    op->batch_id = batch_id;
  } else if (batch_id != op->batch_id) {
    return ge_status_make(GE_STATUS_INVALID_ARGUMENT, "interleaved batch");
  }
  if (op->batch_len >= ASYNC_MAX_BATCH) return ge_status_make(GE_STATUS_RESOURCE_EXHAUSTED, "batch");
  long gain = json_int(req->parameter_json, "gain", 0);
  long fail_at = json_int(req->parameter_json, "fail_at", 0);
  long drop_at = json_int(req->parameter_json, "drop_at", 0);
  long submit_fail_at = json_int(req->parameter_json, "submit_fail_at", 0);
  ++op->seen;
  if (submit_fail_at != 0 && op->seen == submit_fail_at) {
    return ge_status_make(GE_STATUS_INTERNAL, "submit_fail_at reached");
  }
  async_member* m = &op->batch[op->batch_len++];
  memset(m, 0, sizeof(*m));
  ge_completion_event_init(&m->event, req);
  m->port = "out";
  ge_packet_view_init(&m->out);
  m->drop = drop_at != 0 && op->seen == drop_at;
  if (fail_at != 0 && op->seen == fail_at) {
    m->event.status = ge_status_make(GE_STATUS_INTERNAL, "fail_at reached");
  }
  /* Copy the first data input into a fresh buffer (inputs are only valid
   * during submit unless retained); stamp gain into byte 7. */
  for (uint32_t i = 0; i < req->input_count; ++i) {
    const ge_packet_view* in = &req->inputs[i];
    if (in->flags & GE_PACKET_FLAG_EVENT) continue;
    m->out = *in;
    m->out.payload = 0;
    m->out.type_tag = "Bytes";
    m->out.metadata_json = 0;
    if (in->payload != 0) {
      ge_buffer_view view;
      view.header.struct_size = (uint32_t)sizeof(ge_buffer_view);
      view.header.abi_major = GE_ABI_MAJOR;
      if (op->api->buffer_get_view(in->payload, &view).code == GE_STATUS_OK) {
        ge_buffer_handle nb = 0;
        ge_status st = op->api->buffer_alloc(op->host_context, view.size, GE_MEMORY_HOST, -1, &nb);
        if (st.code != GE_STATUS_OK) return st;
        ge_buffer_view nv = view;
        op->api->buffer_get_view(nb, &nv);
        memcpy(nv.data, view.data, view.size);
        if (view.size >= 8) ((unsigned char*)nv.data)[7] = (unsigned char)gain;
        m->out.payload = nb;
        m->payload = nb;
      }
    }
    break;
  }
  if (batch_index + 1 == batch_size) async_flush(op);
  return ge_status_ok();
}

static ge_status sample_process(ge_operator_handle h, const ge_process_request* req) {
  sample_op* op = (sample_op*)h;
  if (!op->opened) return ge_status_make(GE_STATUS_INTERNAL, "process before open");
  switch (op->kind) {
    case 0: return src_process(op, req);
    case 1: return pass_process(op, req);
    case 3:
      if (req->flags & GE_PROCESS_FLAG_FLUSH) {
        async_flush(op);
        return ge_status_ok();
      }
      return ge_status_make(GE_STATUS_INVALID_ARGUMENT, "async operator only accepts submit");
    default: return sink_process(op, req);
  }
}

static ge_status sample_close(ge_operator_handle h, const ge_close_request* req) {
  sample_op* op = (sample_op*)h;
  op->closed = 1;
  op->fast_close = req->fast_shutdown;
  if (op->kind == 3 && op->batch_len != 0) async_flush(op);
  return ge_status_ok();
}

static void sample_destroy(ge_operator_handle h) { free(h); }

/* ------------------------------------------------------------------------- */

static const ge_operator_vtable kSrcVtable =
    GE_OPERATOR_VTABLE_INIT(src_create, sample_open, sample_process, 0, sample_close, sample_destroy);
static const ge_operator_vtable kPassVtable =
    GE_OPERATOR_VTABLE_INIT(pass_create, sample_open, sample_process, 0, sample_close, sample_destroy);
static const ge_operator_vtable kSinkVtable =
    GE_OPERATOR_VTABLE_INIT(sink_create, sample_open, sample_process, 0, sample_close, sample_destroy);
static const ge_operator_vtable kAsyncVtable =
    GE_OPERATOR_VTABLE_INIT(async_create, sample_open, sample_process, async_submit, sample_close,
                            sample_destroy);

#define CAP_HEAD "{\"kind\":\"CapabilityDescriptor\",\"schema_version\":1,"

static const char kSrcCap[] = CAP_HEAD
    "\"operator\":\"Src@" VER "\","
    "\"ports\":{\"inputs\":[],\"outputs\":[{\"name\":\"out\",\"type_tag\":\"Bytes\",\"cardinality\":\"multi\"}]},"
    "\"execution\":{\"stateful\":true,\"max_parallelism\":1}}";

static const char kPassCap[] = CAP_HEAD
    "\"operator\":\"Pass@" VER "\","
    "\"ports\":{\"inputs\":[{\"name\":\"in\",\"type_tag\":\"Bytes\"}],"
    "\"outputs\":[{\"name\":\"out\",\"type_tag\":\"Bytes\",\"cardinality\":\"multi\"}]},"
    "\"execution\":{\"stateful\":false,\"max_parallelism\":1},"
    "\"parameters\":{\"hot_updatable\":[\"gain\",\"fail_at\"],\"migratable\":[\"gain\"]}}";

static const char kSinkCap[] = CAP_HEAD
    "\"operator\":\"Sink@" VER "\","
    "\"ports\":{\"inputs\":[{\"name\":\"in\",\"type_tag\":\"Bytes\"}],\"outputs\":[]},"
    "\"execution\":{\"stateful\":false,\"max_parallelism\":1}}";

static const char kAsyncCap[] = CAP_HEAD
    "\"operator\":\"AsyncPass@" VER "\","
    "\"ports\":{\"inputs\":[{\"name\":\"in\",\"type_tag\":\"Bytes\"}],"
    "\"outputs\":[{\"name\":\"out\",\"type_tag\":\"Bytes\",\"cardinality\":\"multi\"}]},"
    "\"execution\":{\"stateful\":false,\"async\":true,\"max_parallelism\":1,\"max_inference_ms\":200},"
    "\"parameters\":{\"hot_updatable\":[\"gain\",\"fail_at\",\"drop_at\",\"submit_fail_at\"],\"migratable\":[\"gain\"]}}";

static const ge_operator_descriptor kOperators[] = {
    GE_OPERATOR_DESCRIPTOR_INIT("Src", VER, kSrcCap, 0, "counting source", &kSrcVtable,
                                GE_OPERATOR_FLAG_STATEFUL | GE_OPERATOR_FLAG_NO_OWNED_THREADS, 1, 0),
    GE_OPERATOR_DESCRIPTOR_INIT("Pass", VER, kPassCap, 0, "pass-through with gain", &kPassVtable,
                                GE_OPERATOR_FLAG_HOT_UPDATE | GE_OPERATOR_FLAG_NO_OWNED_THREADS, 1, 0),
    GE_OPERATOR_DESCRIPTOR_INIT("Sink", VER, kSinkCap, 0, "counting sink", &kSinkVtable,
                                GE_OPERATOR_FLAG_NO_OWNED_THREADS, 1, 0),
    GE_OPERATOR_DESCRIPTOR_INIT("AsyncPass", VER, kAsyncCap, 0, "fake async pass-through",
                                &kAsyncVtable,
                                GE_OPERATOR_FLAG_ASYNC | GE_OPERATOR_FLAG_HOT_UPDATE |
                                    GE_OPERATOR_FLAG_NO_OWNED_THREADS,
                                1, 200),
};

#define OPERATOR_COUNT 4

static const ge_plugin_descriptor kPlugin =
    GE_PLUGIN_DESCRIPTOR_INIT(PLUGIN_ID, 0, kOperators, OPERATOR_COUNT);

GE_EXPORT const ge_plugin_descriptor* ge_plugin_get_descriptor(uint32_t requested_abi_major) {
  if (requested_abi_major != GE_ABI_MAJOR) return 0;
  return &kPlugin;
}

GE_EXPORT ge_status ge_plugin_create_operator(const ge_operator_create_args* args,
                                              ge_operator_handle* out_operator) {
  if (args == 0 || args->operator_key == 0) return ge_status_make(GE_STATUS_INVALID_ARGUMENT, "args");
  for (uint32_t i = 0; i < OPERATOR_COUNT; ++i) {
    char key[64];
    snprintf(key, sizeof(key), "%s@%s", kOperators[i].type_name, kOperators[i].semantic_version);
    if (strcmp(key, args->operator_key) == 0) return kOperators[i].vtable->create(args, out_operator);
  }
  return ge_status_make(GE_STATUS_NOT_FOUND, "unknown operator");
}
