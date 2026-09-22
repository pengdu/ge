// Minimal install-tree C ABI plugin sample.
// It includes only <ge/c/ge_plugin_sdk.h>, links no GE library, and exports
// ge_plugin_get_descriptor. Build it with either the exported CMake package
// (ge::ge_headers) or the exported pkg-config cflags.

#include <stdlib.h>

#include <ge/c/ge_plugin_sdk.h>

typedef struct install_sink {
  const ge_host_api* api;
  void* host_context;
} install_sink;

static ge_status install_create(const ge_operator_create_args* args, ge_operator_handle* out) {
  install_sink* state = (install_sink*)calloc(1, sizeof(*state));
  if (state == NULL) return ge_status_make(GE_STATUS_RESOURCE_EXHAUSTED, "allocation failed");
  state->api = args->host_api;
  state->host_context = args->host_context;
  *out = (ge_operator_handle)state;
  return ge_status_ok();
}

static ge_status install_open(ge_operator_handle handle, const ge_open_request* request) {
  (void)handle;
  (void)request;
  return ge_status_ok();
}

static ge_status install_process(ge_operator_handle handle, const ge_process_request* request) {
  install_sink* state = (install_sink*)handle;
  if (request->input_count != 0 && state->api->log != NULL) {
    ge_log_record record = {GE_STRUCT_INIT(ge_log_record), GE_SEVERITY_INFO, 0, 0,
                            "InstallSink received a packet", NULL};
    state->api->log(&record);
  }
  return ge_status_ok();
}

static ge_status install_close(ge_operator_handle handle, const ge_close_request* request) {
  (void)handle;
  (void)request;
  return ge_status_ok();
}

static void install_destroy(ge_operator_handle handle) { free((void*)handle); }

static const ge_operator_vtable kInstallVtable =
    GE_OPERATOR_VTABLE_INIT(install_create, install_open, install_process, 0, install_close, install_destroy);

static const char kInstallCapability[] =
    "{\"kind\":\"CapabilityDescriptor\",\"schema_version\":1,"
    "\"operator\":\"InstallSink@1.0.0\","
    "\"ports\":{\"inputs\":[{\"name\":\"in\",\"type_tag\":\"Bytes\"}],\"outputs\":[]},"
    "\"execution\":{\"stateful\":true,\"max_parallelism\":1}}";

static const ge_operator_descriptor kInstallOperators[] = {
    GE_OPERATOR_DESCRIPTOR_INIT("InstallSink", "1.0.0", kInstallCapability, 0, "install sink", &kInstallVtable,
                                GE_OPERATOR_FLAG_NO_OWNED_THREADS, 1, 0),
};

static const ge_plugin_descriptor kInstallPlugin = {
    GE_STRUCT_INIT(ge_plugin_descriptor),
    "com.example.install-sink",
    GE_BUILD_FINGERPRINT,
    0,
    kInstallOperators,
    1,
};

GE_EXPORT const ge_plugin_descriptor* ge_plugin_get_descriptor(uint32_t requested_abi_major) {
  return requested_abi_major == GE_ABI_MAJOR ? &kInstallPlugin : 0;
}
