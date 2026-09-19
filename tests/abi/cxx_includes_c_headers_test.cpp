// Verifies every public C header can be included from a C++20 translation
// unit and that the C types remain POD-compatible.
#include <ge/c/ge_abi.h>
#include <ge/c/ge_engine.h>
#include <ge/c/ge_plugin.h>

#include <type_traits>

static_assert(std::is_standard_layout_v<ge_struct_header>);
static_assert(std::is_standard_layout_v<ge_status>);
static_assert(std::is_standard_layout_v<ge_buffer_view>);
static_assert(std::is_standard_layout_v<ge_packet_view>);
static_assert(std::is_standard_layout_v<ge_event>);
static_assert(std::is_standard_layout_v<ge_log_record>);
static_assert(std::is_standard_layout_v<ge_operator_descriptor>);
static_assert(std::is_standard_layout_v<ge_plugin_descriptor>);
static_assert(std::is_standard_layout_v<ge_host_api>);
static_assert(std::is_standard_layout_v<ge_operator_create_args>);
static_assert(std::is_standard_layout_v<ge_open_request>);
static_assert(std::is_standard_layout_v<ge_close_request>);
static_assert(std::is_standard_layout_v<ge_process_request>);
static_assert(std::is_standard_layout_v<ge_submit_request>);
static_assert(std::is_standard_layout_v<ge_completion_event>);
static_assert(std::is_standard_layout_v<ge_operator_vtable>);
static_assert(std::is_standard_layout_v<ge_engine_config>);
static_assert(std::is_standard_layout_v<ge_event_filter>);

static_assert(std::is_trivially_copyable_v<ge_status>);
static_assert(std::is_trivially_copyable_v<ge_packet_view>);
static_assert(std::is_trivially_copyable_v<ge_completion_event>);

static_assert(offsetof(ge_status, header) == 0);
static_assert(offsetof(ge_packet_view, header) == 0);
static_assert(offsetof(ge_operator_vtable, header) == 0);

int main() {
  ge_status status = {};
  status.header.struct_size = sizeof(ge_status);
  status.header.abi_major = GE_ABI_MAJOR;
  return status.code == GE_STATUS_OK ? 0 : 1;
}
