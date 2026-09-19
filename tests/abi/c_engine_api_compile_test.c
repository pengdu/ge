#include <ge/c/ge_engine.h>

int main(void) {
  ge_engine_config config = {
      .header = GE_STRUCT_HEADER_INIT(ge_engine_config),
      .plugin_search_paths_json = "[]",
      .resource_limits_json = "{}",
      .observability_config_json = "{}",
  };
  ge_event_filter filter = {
      .header = GE_STRUCT_HEADER_INIT(ge_event_filter),
      .filter_json = "{}",
  };
  ge_event event = {
      .header = GE_STRUCT_HEADER_INIT(ge_event),
      .event_id = 1,
      .kind = GE_EVENT_OBSERVATION,
      .type = "eos",
      .severity = GE_SEVERITY_INFO,
      .session_id = 1,
      .source_node_id = 1,
      .timestamp_ns = 0,
      .has_seq = 0,
      .seq = 0,
      .has_pts = 0,
      .pts_ns = 0,
      .detail_json = "{}",
  };
  ge_event_callback callback = 0;
  return config.header.abi_major == GE_ABI_MAJOR &&
                 filter.header.struct_size == sizeof(ge_event_filter) &&
                 event.kind == GE_EVENT_OBSERVATION && callback == 0
             ? 0
             : 1;
}
