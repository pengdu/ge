#include <ge/c/ge_abi.h>

int main(void) {
  ge_struct_header header = {
      .struct_size = sizeof(ge_struct_header),
      .abi_major = GE_ABI_MAJOR,
  };
  return header.abi_major == GE_ABI_MAJOR ? 0 : 1;
}
