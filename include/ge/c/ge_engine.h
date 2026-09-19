#ifndef GE_C_GE_ENGINE_H_
#define GE_C_GE_ENGINE_H_

#include <ge/c/ge_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ge_engine_config {
  ge_struct_header header;
  const char* plugin_search_paths_json;
  const char* resource_limits_json;
  const char* observability_config_json;
} ge_engine_config;

GE_EXPORT ge_status ge_engine_create(const ge_engine_config* config,
                                     ge_engine_handle* out_engine);
GE_EXPORT void ge_engine_destroy(ge_engine_handle engine);
GE_EXPORT ge_status ge_engine_load_plugin(ge_engine_handle engine,
                                          const char* manifest_path,
                                          ge_plugin_id* out_plugin_id);
GE_EXPORT ge_status ge_engine_retire_plugin(ge_engine_handle engine,
                                            ge_plugin_id plugin_id,
                                            uint8_t request_physical_unload,
                                            ge_operation_id* out_operation_id);
GE_EXPORT ge_status ge_engine_upgrade_operator(ge_engine_handle engine,
                                               const char* old_operator_key,
                                               const char* new_operator_key,
                                               ge_operation_id* out_operation_id);

GE_EXPORT ge_status ge_session_create(ge_engine_handle engine,
                                      const char* graph_spec_json,
                                      const char* caller_context_json,
                                      ge_session_handle* out_session,
                                      ge_operation_id* out_operation_id);
GE_EXPORT ge_status ge_session_start(ge_session_handle session);
GE_EXPORT ge_status ge_session_pause(ge_session_handle session);
GE_EXPORT ge_status ge_session_resume(ge_session_handle session);
GE_EXPORT ge_status ge_session_stop(ge_session_handle session,
                                    uint8_t fast_shutdown,
                                    ge_operation_id* out_operation_id);
GE_EXPORT void ge_session_destroy(ge_session_handle session);
GE_EXPORT ge_status ge_session_get_snapshot_json(ge_session_handle session,
                                                 char** out_json);
GE_EXPORT void ge_string_free(ge_engine_handle engine, char* str);

GE_EXPORT ge_status ge_session_dry_run_patch(ge_session_handle session,
                                             const char* patch_json,
                                             char** out_result_json);
GE_EXPORT ge_status ge_session_apply_patch(ge_session_handle session,
                                           const char* patch_json,
                                           const char* caller_context_json,
                                           ge_operation_id* out_operation_id);
GE_EXPORT ge_status ge_session_set_node_parameters(
    ge_session_handle session, ge_node_id node_id, const char* parameters_json,
    const char* caller_context_json, ge_parameter_version* out_version,
    ge_operation_id* out_operation_id);

typedef void (*ge_event_callback)(const ge_event* event, void* user_data);

typedef struct ge_event_filter {
  ge_struct_header header;
  const char* filter_json;
} ge_event_filter;

GE_EXPORT ge_status ge_session_subscribe_events(
    ge_session_handle session, const ge_event_filter* filter,
    ge_event_callback callback, void* user_data,
    ge_subscription_handle* out_subscription);
GE_EXPORT void ge_subscription_cancel(ge_subscription_handle subscription);
GE_EXPORT ge_status ge_engine_get_operation_json(ge_engine_handle engine,
                                                 ge_operation_id operation,
                                                 char** out_json);
GE_EXPORT ge_status ge_engine_get_capability_json(ge_engine_handle engine,
                                                  const char* operator_key,
                                                  char** out_json);

#ifdef __cplusplus
}
#endif

#endif
