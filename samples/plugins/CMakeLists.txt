# Sample plugins (15 P4 task 8), negative ABI variants and the ABI CI tools. Every plugin lands in
# ${GE_OUTPUT_ROOT}/plugins together with a manifest stamped with the engine's build fingerprint.

set (GE_PLUGIN_DIR "${GE_OUTPUT_ROOT}/plugins")
set (GE_MANIFEST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/manifests")

add_executable (ge_fingerprint ${CMAKE_SOURCE_DIR}/tools/ge_fingerprint.c)
target_link_libraries (ge_fingerprint PRIVATE ge_headers)

add_executable (ge_abi_check ${CMAKE_SOURCE_DIR}/tools/ge_abi_check.cpp)
target_link_libraries (ge_abi_check PRIVATE ge_core)

set (GE_STAMPED_MANIFESTS)
function (ge_stamp_manifest name)
  set (out "${GE_PLUGIN_DIR}/${name}")
  add_custom_command (
    OUTPUT "${out}"
    COMMAND ${CMAKE_COMMAND} -DFINGERPRINT_TOOL=$<TARGET_FILE:ge_fingerprint> -DTEMPLATE=${GE_MANIFEST_SRC}/${name}.in
            -DOUTPUT=${out} -P ${CMAKE_SOURCE_DIR}/cmake/stamp_manifest.cmake
    DEPENDS ge_fingerprint "${GE_MANIFEST_SRC}/${name}.in" ${CMAKE_SOURCE_DIR}/cmake/stamp_manifest.cmake
    COMMENT "Stamping ${name}")
  list (APPEND GE_STAMPED_MANIFESTS "${out}")
  set (
    GE_STAMPED_MANIFESTS
    "${GE_STAMPED_MANIFESTS}"
    PARENT_SCOPE)
endfunction ()

ge_add_plugin (sample_plugin sample_plugin.c)
ge_add_plugin (sample_plugin_v2 sample_plugin.c)
target_compile_definitions (sample_plugin_v2 PRIVATE GE_SAMPLE_VERSION_2=1)

function (ge_add_bad_plugin name define)
  ge_add_plugin (${name} bad_plugin.c)
  if (NOT define STREQUAL "")
    target_compile_definitions (${name} PRIVATE ${define}=1)
  endif ()
endfunction ()
ge_add_bad_plugin (bad_ok "")
ge_add_bad_plugin (bad_abi_major GE_BAD_ABI_MAJOR)
ge_add_bad_plugin (bad_header GE_BAD_HEADER)
ge_add_bad_plugin (bad_operators GE_BAD_OPERATORS)
ge_add_bad_plugin (bad_vtable GE_BAD_VTABLE)
ge_add_bad_plugin (bad_fingerprint GE_BAD_FINGERPRINT)
ge_add_bad_plugin (no_symbol GE_NO_SYMBOL)
ge_add_bad_plugin (bad_parallelism GE_BAD_PARALLELISM)

foreach (
  m
  sample_plugin.json
  sample_plugin_v2.json
  bad_ok.json
  bad_abi_major.json
  bad_header.json
  bad_operators.json
  bad_vtable.json
  bad_fingerprint.json
  no_symbol.json
  bad_parallelism.json
  bad_manifest_abi.json
  bad_dependency.json
  bad_manifest_fingerprint.json
  bad_missing_library.json)
  ge_stamp_manifest (${m})
endforeach ()

add_custom_target (ge_sample_manifests ALL DEPENDS ${GE_STAMPED_MANIFESTS})
add_dependencies (ge_sample_manifests sample_plugin sample_plugin_v2 bad_ok bad_abi_major bad_header bad_operators
                  bad_vtable bad_fingerprint no_symbol bad_parallelism)

# ABI CI checks (15 P4 task 8): good manifests must pass, bad ones must be rejected by the dry-run loader.
if (BUILD_TESTING)
  add_test (NAME ge_abi_check_good
            COMMAND ge_abi_check --search-path "${GE_PLUGIN_DIR}" "${GE_PLUGIN_DIR}/sample_plugin.json"
                    "${GE_PLUGIN_DIR}/sample_plugin_v2.json" "${GE_PLUGIN_DIR}/bad_ok.json")
  add_test (
    NAME ge_abi_check_bad
    COMMAND
      ge_abi_check --expect-fail --search-path "${GE_PLUGIN_DIR}" "${GE_PLUGIN_DIR}/bad_abi_major.json"
      "${GE_PLUGIN_DIR}/bad_header.json" "${GE_PLUGIN_DIR}/bad_operators.json" "${GE_PLUGIN_DIR}/bad_vtable.json"
      "${GE_PLUGIN_DIR}/bad_fingerprint.json" "${GE_PLUGIN_DIR}/no_symbol.json" "${GE_PLUGIN_DIR}/bad_parallelism.json"
      "${GE_PLUGIN_DIR}/bad_manifest_abi.json" "${GE_PLUGIN_DIR}/bad_dependency.json"
      "${GE_PLUGIN_DIR}/bad_manifest_fingerprint.json" "${GE_PLUGIN_DIR}/bad_missing_library.json")
endif ()
