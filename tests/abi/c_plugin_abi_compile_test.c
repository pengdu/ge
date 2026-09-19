#include <ge/c/ge_plugin.h>

int main(void) {
  ge_operator_vtable vtable = {
      .header = {.struct_size = sizeof(ge_operator_vtable), .abi_major = GE_ABI_MAJOR},
      .create = 0,
      .open = 0,
      .process = 0,
      .submit = 0,
      .close = 0,
      .destroy = 0,
  };
  ge_operator_descriptor descriptor = {
      .header = {.struct_size = sizeof(ge_operator_descriptor), .abi_major = GE_ABI_MAJOR},
      .type_name = "Pass",
      .semantic_version = "1.0.0",
      .capability_json = "{}",
      .parameter_schema_json = "{}",
      .functional_description = "pass through",
      .vtable = &vtable,
      .flags = 0,
      .max_parallelism = 1,
      .max_inference_ms = 0,
  };
  return descriptor.vtable == &vtable ? 0 : 1;
}
