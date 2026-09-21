# ---------------------------------------------------------------------------
# GoogleTest discovery. Prefer a toolchain-compatible archive; on this machine the /usr/local/lib pair links correctly
# with the system clang.
# ---------------------------------------------------------------------------
find_library (
  GTEST_LIBRARY
  NAMES gtest
  PATHS /usr/local/lib /opt/homebrew/lib /usr/lib
  NO_DEFAULT_PATH)
find_library (
  GTEST_MAIN_LIBRARY
  NAMES gtest_main
  PATHS /usr/local/lib /opt/homebrew/lib /usr/lib
  NO_DEFAULT_PATH)
if (NOT GTEST_LIBRARY)
  find_library (GTEST_LIBRARY NAMES gtest)
endif ()
if (NOT GTEST_MAIN_LIBRARY)
  find_library (GTEST_MAIN_LIBRARY NAMES gtest_main)
endif ()
find_path (GTEST_INCLUDE_DIR gtest/gtest.h PATHS /usr/local/include /opt/homebrew/include /usr/include)
if (NOT GTEST_LIBRARY
    OR NOT GTEST_MAIN_LIBRARY
    OR NOT GTEST_INCLUDE_DIR)
  message (FATAL_ERROR "GTest not found. Install googletest or set GTEST_LIBRARY/GTEST_MAIN_LIBRARY/GTEST_INCLUDE_DIR.")
endif ()

add_library (GTest::gtest UNKNOWN IMPORTED)
set_target_properties (GTest::gtest PROPERTIES IMPORTED_LOCATION "${GTEST_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES
                                                                                    "${GTEST_INCLUDE_DIR}")
add_library (GTest::gtest_main UNKNOWN IMPORTED)
set_target_properties (GTest::gtest_main PROPERTIES IMPORTED_LOCATION "${GTEST_MAIN_LIBRARY}"
                                                    INTERFACE_INCLUDE_DIRECTORIES "${GTEST_INCLUDE_DIR}")

# gtest headers trip a few of our strict warnings; silence them for tests only. Tests also lean on designated
# initialisers that omit members without a default member initialiser ({.id = "e0"} for EdgeOptions), which GCC and
# clang <= 18 flag under -Wextra (Apple clang 21 does not).
add_library (ge_test_flags INTERFACE)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options (ge_test_flags INTERFACE -Wno-conversion -Wno-sign-conversion -Wno-shadow
                                                  -Wno-missing-field-initializers)
  if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC -O3 inlines std::variant's destructor visitor for every alternative and then cannot prove which one is
    # live (Result<int> in types_test), reporting the inactive std::string as maybe-uninitialized. Known libstdc++
    # false positive (PR 80635 family); src/ compiles clean, so it is silenced for tests only.
    target_compile_options (ge_test_flags INTERFACE -Wno-maybe-uninitialized)
  endif ()
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang 21+ only; an unknown -Wno- flag is itself an error under -Werror on older clang.
    include (CheckCXXCompilerFlag)
    check_cxx_compiler_flag (-Wcharacter-conversion GE_HAS_WCHARACTER_CONVERSION)
    if (GE_HAS_WCHARACTER_CONVERSION)
      target_compile_options (ge_test_flags INTERFACE -Wno-character-conversion)
    endif ()
  endif ()
endif ()

# C ABI compile tests: each C header must compile as C11 and as C++20.
function (ge_add_c_compile_test name source)
  add_executable (${name} ${source})
  target_link_libraries (${name} PRIVATE ge_headers)
  add_test (NAME ${name} COMMAND ${name})
endfunction ()

ge_add_c_compile_test (ge_c_abi_compile_test abi/c_abi_compile_test.c)
ge_add_c_compile_test (ge_c_status_compile_test abi/c_status_compile_test.c)
ge_add_c_compile_test (ge_c_packet_compile_test abi/c_packet_compile_test.c)
ge_add_c_compile_test (ge_c_plugin_abi_compile_test abi/c_plugin_abi_compile_test.c)
ge_add_c_compile_test (ge_c_host_api_compile_test abi/c_host_api_compile_test.c)
ge_add_c_compile_test (ge_c_engine_api_compile_test abi/c_engine_api_compile_test.c)
ge_add_c_compile_test (ge_cxx_includes_c_headers_test abi/cxx_includes_c_headers_test.cpp)

# gtest-based unit tests.
function (ge_add_unit_test name)
  add_executable (${name} ${ARGN})
  target_link_libraries (${name} PRIVATE ge_core ge_test_flags GTest::gtest GTest::gtest_main)
  add_test (NAME ${name} COMMAND ${name})
endfunction ()

ge_add_unit_test (ge_types_test unit/types_test.cpp)
ge_add_unit_test (ge_json_test unit/json_test.cpp)
ge_add_unit_test (ge_graph_spec_test unit/graph_spec_test.cpp)
ge_add_unit_test (ge_capability_test unit/capability_test.cpp)
ge_add_unit_test (ge_graph_validator_test unit/graph_validator_test.cpp)
ge_add_unit_test (ge_plugin_manifest_test unit/plugin_manifest_test.cpp)
ge_add_unit_test (ge_mutation_applier_test unit/mutation_applier_test.cpp)
ge_add_unit_test (ge_graph_diff_test unit/graph_diff_test.cpp)
ge_add_unit_test (ge_resource_ledger_test unit/resource_ledger_test.cpp)
ge_add_unit_test (ge_packet_edge_test unit/packet_edge_test.cpp)
ge_add_unit_test (ge_input_binding_test unit/input_binding_test.cpp)
ge_add_unit_test (ge_scheduler_test unit/scheduler_test.cpp)
ge_add_unit_test (ge_session_test unit/session_test.cpp)
ge_add_unit_test (ge_mutation_stress_test unit/mutation_stress_test.cpp)
ge_add_unit_test (ge_async_runtime_test unit/async_runtime_test.cpp)
ge_add_unit_test (ge_builtin_operators_test unit/builtin_operators_test.cpp)

# Inference scene library (ge_infer).
ge_add_unit_test (ge_flow_limiter_test unit/flow_limiter_test.cpp)
target_link_libraries (ge_flow_limiter_test PRIVATE ge_infer)

# OnnxInfer against a real model. The model is fetched once into the source-tree model cache (git-ignored); an
# offline machine gets the test compiled with an empty path and it reports SKIPPED.
set (GE_MODEL_DIR "${CMAKE_SOURCE_DIR}/build/models")
set (GE_MOBILENET_ONNX "${GE_MODEL_DIR}/mobilenetv2-12.onnx")
if (GE_ENABLE_ONNX AND NOT EXISTS "${GE_MOBILENET_ONNX}")
  file (MAKE_DIRECTORY "${GE_MODEL_DIR}")
  message (STATUS "ge_infer: fetching mobilenetv2-12.onnx (14 MB) ...")
  file (
    DOWNLOAD
    "https://github.com/onnx/models/raw/main/validated/vision/classification/mobilenet/model/mobilenetv2-12.onnx"
    "${GE_MOBILENET_ONNX}.part"
    STATUS GE_DL_STATUS
    TIMEOUT 300)
  list (GET GE_DL_STATUS 0 GE_DL_CODE)
  if (GE_DL_CODE EQUAL 0)
    file (RENAME "${GE_MOBILENET_ONNX}.part" "${GE_MOBILENET_ONNX}")
  else ()
    file (REMOVE "${GE_MOBILENET_ONNX}.part")
    message (STATUS "ge_infer: model download failed (${GE_DL_STATUS}); ge_onnx_infer_test will skip")
  endif ()
endif ()
ge_add_unit_test (ge_onnx_infer_test unit/onnx_infer_test.cpp)
target_link_libraries (ge_onnx_infer_test PRIVATE ge_infer)
if (GE_ENABLE_ONNX)
  # The test runs the reference inference through ORT directly.
  target_compile_definitions (ge_onnx_infer_test PRIVATE GE_HAVE_ONNXRUNTIME_TEST=1)
  target_include_directories (ge_onnx_infer_test SYSTEM PRIVATE ${ONNXRUNTIME_INCLUDE_DIRS})
  target_link_libraries (ge_onnx_infer_test PRIVATE PkgConfig::ONNXRUNTIME)
else ()
  target_compile_definitions (ge_onnx_infer_test PRIVATE GE_HAVE_ONNXRUNTIME_TEST=0)
endif ()
if (EXISTS "${GE_MOBILENET_ONNX}")
  target_compile_definitions (ge_onnx_infer_test PRIVATE "GE_MOBILENET_ONNX=\"${GE_MOBILENET_ONNX}\"")
else ()
  target_compile_definitions (ge_onnx_infer_test PRIVATE "GE_MOBILENET_ONNX=\"\"")
endif ()
set_tests_properties (ge_onnx_infer_test PROPERTIES LABELS infer TIMEOUT 300)

# Tests that dlopen the sample plugins (built in samples/plugins).
function (ge_add_plugin_unit_test name)
  ge_add_unit_test (${name} ${ARGN})
  target_compile_definitions (${name} PRIVATE "GE_PLUGIN_DIR=\"${GE_OUTPUT_ROOT}/plugins\"")
  add_dependencies (${name} ge_sample_manifests)
endfunction ()

ge_add_plugin_unit_test (ge_plugin_registry_test unit/plugin_registry_test.cpp)
ge_add_plugin_unit_test (ge_engine_test unit/engine_test.cpp)

# P6 media acceptance (needs FFmpeg through ge_media).
if (GE_ENABLE_MEDIA)
  ge_add_unit_test (ge_media_transcode_test unit/media_transcode_test.cpp)
  target_link_libraries (ge_media_transcode_test PRIVATE ge_media)
  set_tests_properties (ge_media_transcode_test PROPERTIES LABELS media TIMEOUT 300)
endif ()

# Contention stress (15 §12.1). Every binary below is run as GE_STRESS_PARALLEL concurrent copies, each repeating its
# suite GE_STRESS_REPEAT times, for GE_STRESS_ROUNDS rounds: liveness bugs in the scheduler only surface when the
# executor threads are starved of CPU. Labelled "stress" so the regular run excludes them (ctest -LE stress) and the
# nightly job selects them (ctest -L stress).
set (GE_STRESS_PARALLEL 8 CACHE STRING "stress: concurrent copies per binary")
set (GE_STRESS_REPEAT 10 CACHE STRING "stress: --gtest_repeat per copy")
set (GE_STRESS_ROUNDS 3 CACHE STRING "stress: rounds")
set (GE_STRESS_TIMEOUT 600 CACHE STRING "stress: per-copy watchdog seconds")
function (ge_add_stress_test target)
  set (name ${target}_stress)
  add_test (NAME ${name}
            COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/stress/run_stress.sh" "$<TARGET_FILE:${target}>" ${GE_STRESS_PARALLEL}
                    ${GE_STRESS_REPEAT} ${GE_STRESS_ROUNDS} ${GE_STRESS_TIMEOUT} ${ARGN})
  math (EXPR stress_total "${GE_STRESS_TIMEOUT} * ${GE_STRESS_ROUNDS} + 60")
  set_tests_properties (${name} PROPERTIES LABELS stress TIMEOUT ${stress_total} RUN_SERIAL TRUE)
endfunction ()
# The 10-node/20k-packet latency probe runs inline (no executor threads), so contention teaches it nothing; under TSan
# it costs ~15s per repeat and pushed the copies past the watchdog.
ge_add_stress_test (ge_scheduler_test "-*TenNodeLatency*")
ge_add_stress_test (ge_mutation_stress_test)
ge_add_stress_test (ge_async_runtime_test)
ge_add_stress_test (ge_session_test)
ge_add_stress_test (ge_flow_limiter_test)
ge_add_stress_test (ge_onnx_infer_test "*Unbatched*:*Batched*:*TwoSessions*:*FastStop*")

# Performance baselines (15 §4/§5). Registered as a test so CI keeps the numbers; the budgets are only enforced on
# optimised, non-sanitized builds.
add_executable (ge_bench bench/ge_bench.cpp)
target_link_libraries (ge_bench PRIVATE ge_core ge_test_flags)
add_test (NAME ge_bench COMMAND ge_bench --iterations 8 --packets 5000 --json)
set_tests_properties (ge_bench PROPERTIES LABELS bench)
