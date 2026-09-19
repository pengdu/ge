#include <ge/c/ge_abi.h>

int main(void) {
  ge_buffer_view buffer = {
      .header = {.struct_size = sizeof(ge_buffer_view), .abi_major = GE_ABI_MAJOR},
      .data = 0,
      .size = 0,
      .memory_kind = GE_MEMORY_HOST,
      .device_id = -1,
  };
  ge_packet_view packet = {
      .header = {.struct_size = sizeof(ge_packet_view), .abi_major = GE_ABI_MAJOR},
      .seq = 1,
      .pts_ns = 0,
      .dts_ns = 0,
      .flags = 0,
      .topology_version = 1,
      .parameter_version = 1,
      .type_tag = "Bytes",
      .metadata_json = "{}",
      .payload = 0,
  };
  return buffer.memory_kind == GE_MEMORY_HOST && packet.seq == 1 ? 0 : 1;
}
