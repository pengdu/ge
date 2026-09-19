#include <ge/c/ge_plugin.h>

int main(void) {
  ge_host_api api = {
      .header = {.struct_size = sizeof(ge_host_api), .abi_major = GE_ABI_MAJOR},
      .buffer_retain = 0,
      .buffer_release = 0,
      .buffer_get_view = 0,
      .emit = 0,
      .completion_push = 0,
      .event_publish = 0,
      .log = 0,
  };
  return api.header.abi_major == GE_ABI_MAJOR ? 0 : 1;
}
