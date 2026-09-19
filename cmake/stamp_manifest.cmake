# Generates a plugin manifest with the engine's build fingerprint stamped
# in. Invoked at build time:
#   cmake -DFINGERPRINT_TOOL=... -DTEMPLATE=... -DOUTPUT=... -P stamp_manifest.cmake
execute_process (COMMAND "${FINGERPRINT_TOOL}" OUTPUT_VARIABLE GE_FINGERPRINT RESULT_VARIABLE rc)
if (NOT rc EQUAL 0)
  message (FATAL_ERROR "fingerprint tool failed: ${rc}")
endif ()
file (READ "${TEMPLATE}" content)
string (REPLACE "@GE_BUILD_FINGERPRINT@" "${GE_FINGERPRINT}" content "${content}")
file (WRITE "${OUTPUT}" "${content}")
