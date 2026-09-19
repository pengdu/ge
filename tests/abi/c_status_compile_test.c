#include <ge/c/ge_abi.h>

int main(void) {
  ge_status status = {
      .header = {.struct_size = sizeof(ge_status), .abi_major = GE_ABI_MAJOR},
      .code = GE_STATUS_CAPABILITY_CONFLICT,
      .retryable = 0,
      .message = "capability conflict",
      .detail_json = "{}",
  };

  return status.code == GE_STATUS_CAPABILITY_CONFLICT && !status.retryable ? 0 : 1;
}
