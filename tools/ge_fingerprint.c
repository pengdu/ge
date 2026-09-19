#include <stdio.h>

#include <ge/c/ge_plugin.h>

/* Prints the engine's toolchain fingerprint (PLG-3); the build uses it to
 * stamp sample manifests. */
int main(void) {
  printf("%s", GE_BUILD_FINGERPRINT);
  return 0;
}
