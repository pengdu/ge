/* Negative plugins for the ABI CI check (15 P4 task 8). Each variant is a
 * separate build of this file:
 *   GE_BAD_ABI_MAJOR    descriptor refuses the engine's ABI major
 *   GE_BAD_HEADER       descriptor header carries abi_major 99
 *   GE_BAD_OPERATORS    descriptor lists an operator the manifest lacks
 *   GE_BAD_VTABLE       vtable lacks process()
 *   GE_BAD_FINGERPRINT  descriptor fingerprint differs from the manifest
 *   GE_NO_SYMBOL        does not export ge_plugin_get_descriptor
 *   GE_BAD_PARALLELISM  descriptor max_parallelism disagrees with capability */

#include <stdlib.h>

#include <ge/c/ge_plugin_sdk.h>

static ge_status nop_create(const ge_operator_create_args* a, ge_operator_handle* o) {
  (void)a;
  *o = (ge_operator_handle)malloc(1);
  return ge_status_ok();
}
static ge_status nop_open(ge_operator_handle h, const ge_open_request* r) {
  (void)h;
  (void)r;
  return ge_status_ok();
}
#ifndef GE_BAD_VTABLE
static ge_status nop_process(ge_operator_handle h, const ge_process_request* r) {
  (void)h;
  (void)r;
  return ge_status_ok();
}
#endif
static ge_status nop_close(ge_operator_handle h, const ge_close_request* r) {
  (void)h;
  (void)r;
  return ge_status_ok();
}
static void nop_destroy(ge_operator_handle h) { free(h); }

#ifdef GE_BAD_VTABLE
static const ge_operator_vtable kVtable =
    GE_OPERATOR_VTABLE_INIT(nop_create, nop_open, 0, 0, nop_close, nop_destroy);
#else
static const ge_operator_vtable kVtable =
    GE_OPERATOR_VTABLE_INIT(nop_create, nop_open, nop_process, 0, nop_close, nop_destroy);
#endif

static const char kCap[] =
    "{\"kind\":\"CapabilityDescriptor\",\"schema_version\":1,"
    "\"operator\":\"Bad@1.0.0\","
    "\"ports\":{\"inputs\":[{\"name\":\"in\",\"type_tag\":\"Bytes\"}],\"outputs\":[]},"
    "\"execution\":{\"stateful\":false,\"max_parallelism\":1}}";

#ifdef GE_BAD_PARALLELISM
#define PAR 4
#else
#define PAR 1
#endif

static const ge_operator_descriptor kOperators[] = {
    GE_OPERATOR_DESCRIPTOR_INIT("Bad", "1.0.0", kCap, 0, "bad", &kVtable,
                                GE_OPERATOR_FLAG_NO_OWNED_THREADS, PAR, 0),
#ifdef GE_BAD_OPERATORS
    GE_OPERATOR_DESCRIPTOR_INIT("Extra", "1.0.0", kCap, 0, "extra", &kVtable,
                                GE_OPERATOR_FLAG_NO_OWNED_THREADS, 1, 0),
#endif
};

#ifdef GE_BAD_FINGERPRINT
#define FP "not-the-engine-fingerprint"
#else
#define FP GE_BUILD_FINGERPRINT
#endif

static const ge_plugin_descriptor kPlugin = {
#ifdef GE_BAD_HEADER
    {(uint32_t)sizeof(ge_plugin_descriptor), 99u},
#else
    GE_STRUCT_INIT(ge_plugin_descriptor),
#endif
    "com.example.bad",
    FP,
    0,
    kOperators,
    (uint32_t)(sizeof(kOperators) / sizeof(kOperators[0])),
};

#ifndef GE_NO_SYMBOL
GE_EXPORT const ge_plugin_descriptor* ge_plugin_get_descriptor(uint32_t requested_abi_major) {
#ifdef GE_BAD_ABI_MAJOR
  /* Pretends to be a plugin of another ABI generation: never hands out the
   * descriptor for this major. */
  return requested_abi_major == 0xFFFFFFFFu ? &kPlugin : 0;
#else
  if (requested_abi_major != GE_ABI_MAJOR) return 0;
  return &kPlugin;
#endif
}
#else
GE_EXPORT const ge_plugin_descriptor* ge_plugin_get_descriptor_renamed(uint32_t m) {
  (void)m;
  return &kPlugin;
}
#endif
