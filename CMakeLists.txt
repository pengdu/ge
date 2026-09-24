cmake_minimum_required (VERSION 3.25)

project (
  graph_engine
  VERSION 0.1.0
  LANGUAGES C CXX)

set (GE_PACKAGE_NAME "ge")
set (GE_PACKAGE_DESCRIPTION "Graph execution engine")
set (GE_MEDIA_PACKAGE_NAME "ge-media")
set (GE_MEDIA_PACKAGE_DESCRIPTION "FFmpeg media operators for the graph execution engine")
set (GE_PACKAGE_PC_PREFIX "\${pcfiledir}/../..")

set (CMAKE_C_STANDARD 11)
set (CMAKE_C_STANDARD_REQUIRED ON)
set (CMAKE_C_EXTENSIONS OFF)
set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)
set (CMAKE_POSITION_INDEPENDENT_CODE ON)
set (CMAKE_CXX_VISIBILITY_PRESET hidden)
set (CMAKE_C_VISIBILITY_PRESET hidden)
set (CMAKE_VISIBILITY_INLINES_HIDDEN ON)

option (GE_ENABLE_SANITIZERS "Build with ASan/UBSan" OFF)
option (GE_ENABLE_TSAN "Build with ThreadSanitizer (exclusive with ASan)" OFF)
option (GE_BUILD_SAMPLES "Build FFmpeg samples (requires FFmpeg)" ON)
option (GE_ENABLE_MEDIA "Build FFmpeg media operators" ON)
option (GE_ENABLE_ONNX "Build OnnxInfer in ge_infer (needs ONNX Runtime via pkg-config)" ON)

# Build artifacts land in build/<platform>/<release|debug>/{lib,bin} so that several platforms and configurations can
# coexist under one source tree.
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set (
    CMAKE_BUILD_TYPE
    Release
    CACHE STRING "Build type" FORCE)
  set_property (CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif ()

string (TOLOWER "${CMAKE_SYSTEM_NAME}" GE_PLATFORM_OS)
string (TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" GE_PLATFORM_ARCH)
set (GE_PLATFORM "${GE_PLATFORM_OS}-${GE_PLATFORM_ARCH}")
# Sanitizer builds get their own output root so they never overwrite (or get picked up instead of) the plain
# Release/Debug artefacts.
# An explicit -DGE_OUTPUT_SUFFIX=... isolates other variants too (e.g. a build against ffmpeg@7).
if (NOT DEFINED GE_OUTPUT_SUFFIX)
  set (GE_OUTPUT_SUFFIX "")
  if (GE_ENABLE_SANITIZERS)
    set (GE_OUTPUT_SUFFIX "-asan")
  elseif (GE_ENABLE_TSAN)
    set (GE_OUTPUT_SUFFIX "-tsan")
  endif ()
endif ()
set (GE_OUTPUT_ROOT "${CMAKE_SOURCE_DIR}/build/${GE_PLATFORM}/$<LOWER_CASE:$<CONFIG>>${GE_OUTPUT_SUFFIX}")

include (GNUInstallDirs)

set (CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${GE_OUTPUT_ROOT}/lib")
set (CMAKE_LIBRARY_OUTPUT_DIRECTORY "${GE_OUTPUT_ROOT}/lib")
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY "${GE_OUTPUT_ROOT}/bin")

# ---------------------------------------------------------------------------
# Warning / sanitizer flags shared by every target.
# ---------------------------------------------------------------------------
# Everything is wrapped in $<BUILD_INTERFACE> so an installed/exported ge
# never imposes -Werror or sanitizer flags on downstream projects.
add_library (ge_build_flags INTERFACE)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options (
    ge_build_flags INTERFACE "$<BUILD_INTERFACE:-Wall;-Wextra;-Wpedantic;-Werror;-Wshadow;-Wconversion;-Wsign-conversion;$<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>>")
  if (GE_ENABLE_SANITIZERS)
    target_compile_options (ge_build_flags INTERFACE "$<BUILD_INTERFACE:-fsanitize=address,undefined;-fno-omit-frame-pointer>")
    target_link_options (ge_build_flags INTERFACE "$<BUILD_INTERFACE:-fsanitize=address,undefined>")
  elseif (GE_ENABLE_TSAN)
    target_compile_options (ge_build_flags INTERFACE "$<BUILD_INTERFACE:-fsanitize=thread;-fno-omit-frame-pointer>")
    target_link_options (ge_build_flags INTERFACE "$<BUILD_INTERFACE:-fsanitize=thread>")
  endif ()
endif ()

# ---------------------------------------------------------------------------
# Public headers (C ABI + C++ wrappers). Header-only consumers link this.
# ---------------------------------------------------------------------------
add_library (ge_headers INTERFACE)
target_include_directories (ge_headers INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                                 $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries (ge_headers INTERFACE ge_build_flags)

# ---------------------------------------------------------------------------
# Core runtime library.
# ---------------------------------------------------------------------------
find_package (Threads REQUIRED)

add_library (
  ge_core STATIC
  src/json.cpp
  src/graph_spec.cpp
  src/graph_spec_json.cpp
  src/graph_template.cpp
  src/batch_runner.cpp
  src/capability.cpp
  src/graph_validator.cpp
  src/graph_diff.cpp
  src/plugin_manifest.cpp
  src/mutation_applier.cpp
  src/packet.cpp
  src/edge_channel.cpp
  src/input_binding.cpp
  src/format_event.cpp
  src/node_runtime.cpp
  src/runtime_topology.cpp
  src/executor_pool.cpp
  src/scheduler.cpp
  src/scheduler_invoke.cpp
  src/scheduler_backpressure.cpp
  src/scheduler_retire.cpp
  src/async_runtime.cpp
  src/operation.cpp
  src/audit_log.cpp
  src/metrics_export.cpp
  src/resource_ledger.cpp
  src/session.cpp
  src/plugin_operator.cpp
  src/plugin_registry.cpp
  src/event_bus.cpp
  src/engine.cpp
  src/session_manager.cpp
  src/plugin_lifecycle_service.cpp
  src/c_api.cpp)
target_include_directories (
  ge_core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_compile_features (ge_core PUBLIC cxx_std_20)
target_link_libraries (ge_core PUBLIC ge_headers Threads::Threads ${CMAKE_DL_LIBS})

# Shared host library exporting the C API (13 §5). Plugins never link it; they only include the headers.
add_library (ge_engine SHARED src/ge_engine_export.cpp)
target_link_libraries (ge_engine PRIVATE $<LINK_LIBRARY:WHOLE_ARCHIVE,ge_core>)
target_link_libraries (ge_engine PUBLIC ge_headers)
set_target_properties (ge_engine PROPERTIES OUTPUT_NAME ge_engine)

# Plugin SDK helper: a plugin is a bare shared module exporting ge_plugin_get_descriptor (13 §4.2).
function (ge_add_plugin name)
  add_library (${name} MODULE ${ARGN})
  target_link_libraries (${name} PRIVATE ge_headers)
  set_target_properties (
    ${name}
    PROPERTIES PREFIX ""
               SUFFIX ".so"
               LIBRARY_OUTPUT_DIRECTORY "${GE_OUTPUT_ROOT}/plugins")
  target_compile_definitions (${name} PRIVATE GE_PLUGIN_BUILD=1)
  # ASan keeps global redzones registered across dlclose; a module reloaded at the same address (DryRunLoad,
  # PhysicalUnload tests) then reports false global-buffer-overflows. Plugins therefore skip global instrumentation
  # under ASan (heap/stack checks stay on).
  if (GE_ENABLE_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options (${name} PRIVATE -mllvm -asan-globals=0)
  endif ()
endfunction ()

# ---------------------------------------------------------------------------
# Inference scene library (OD-09). Depends on ge_core only; no external dependencies.
# ---------------------------------------------------------------------------
add_library (ge_infer STATIC src/infer/flow_limiter.cpp src/infer/tensor.cpp src/infer/onnx_infer.cpp)
target_include_directories (
  ge_infer
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
target_link_libraries (ge_infer PUBLIC ge_core)

# ONNX Runtime is optional: without it OnnxInfer is registered but Open fails with RESOURCE_EXHAUSTED.
if (GE_ENABLE_ONNX)
  find_package (PkgConfig QUIET)
  if (PkgConfig_FOUND)
    pkg_check_modules (ONNXRUNTIME IMPORTED_TARGET libonnxruntime)
  endif ()
  if (NOT ONNXRUNTIME_FOUND)
    message (STATUS "ge_infer: ONNX Runtime not found via pkg-config, GE_ENABLE_ONNX=OFF")
    set (GE_ENABLE_ONNX OFF)
  endif ()
endif ()
if (GE_ENABLE_ONNX)
  message (STATUS "ge_infer: onnxruntime ${ONNXRUNTIME_VERSION}")
  target_compile_definitions (ge_infer PRIVATE GE_HAVE_ONNXRUNTIME=1)
  # ORT headers are not -Wpedantic/-Wconversion clean; treat them as system headers.
  target_include_directories (ge_infer SYSTEM PRIVATE ${ONNXRUNTIME_INCLUDE_DIRS})
  target_link_libraries (ge_infer PUBLIC PkgConfig::ONNXRUNTIME)
else ()
  target_compile_definitions (ge_infer PRIVATE GE_HAVE_ONNXRUNTIME=0)
endif ()

# ---------------------------------------------------------------------------
# Media library (P6). ge_core stays FFmpeg-free; ge_media links it and the FFmpeg libraries found through pkg-config.
# Missing FFmpeg turns the option off with a notice instead of failing the configure step.
# ---------------------------------------------------------------------------
if (GE_ENABLE_MEDIA)
  find_package (PkgConfig QUIET)
  if (PkgConfig_FOUND)
    pkg_check_modules (FFMPEG IMPORTED_TARGET libavcodec libavformat libavutil libswscale libswresample libavfilter)
  endif ()
  if (NOT FFMPEG_FOUND)
    message (STATUS "ge_media: FFmpeg not found via pkg-config, GE_ENABLE_MEDIA=OFF")
    set (GE_ENABLE_MEDIA OFF)
  endif ()
endif ()

if (GE_ENABLE_MEDIA)
  # Floor: FFmpeg 6.0 (libavcodec 60, first release with AVChannelLayout). 6.x / 7.x / 8+ differences are covered by
  # the GE_FF_* macros in src/media/ffmpeg.h.
  if (FFMPEG_libavcodec_VERSION VERSION_LESS 60)
    message (FATAL_ERROR "ge_media requires FFmpeg >= 6.0 (libavcodec >= 60), found ${FFMPEG_libavcodec_VERSION}")
  endif ()
  message (STATUS "ge_media: libavcodec ${FFMPEG_libavcodec_VERSION}")
  add_library (
    ge_media STATIC
    src/media/ffmpeg.cpp
    src/media/frame.cpp
    src/media/device_buffer.cpp
    src/media/capabilities.cpp
    src/media/media_demux.cpp
    src/media/video_decode.cpp
    src/media/video_scale.cpp
    src/media/video_filter.cpp
    src/media/video_encode.cpp
    src/media/audio_encode.cpp
    src/media/media_mux.cpp
    src/media/video_compose.cpp
    src/media/audio_mix.cpp
    src/media/mix.cpp
    src/media/mix_controller.cpp
    src/media/rendition.cpp
    src/media/transcode_controller.cpp
    src/media/testing.cpp)
  target_include_directories (
    ge_media
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
  # FFmpeg headers are not -Wpedantic clean; treat them as system headers.
  target_include_directories (ge_media SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
  target_link_libraries (
    ge_media
    PUBLIC ge_core PkgConfig::FFMPEG)
endif ()

# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------
include (CTest)
if (BUILD_TESTING)
  add_subdirectory (tests)
  configure_file (cmake/verify_install_consumer.cmake.in
                  "${CMAKE_CURRENT_BINARY_DIR}/verify_install_consumer.cmake" @ONLY)
  add_test (NAME ge_install_consumer
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/verify_install_consumer.cmake")
  set_tests_properties (ge_install_consumer PROPERTIES LABELS install TIMEOUT 300)
endif ()

# ---------------------------------------------------------------------------
# Sample plugins + ABI CI tools (always; no external dependencies).
# ---------------------------------------------------------------------------
add_subdirectory (samples/plugins)

# ---------------------------------------------------------------------------
# Samples (optional, need FFmpeg). The sample app lands with the P6 CLI; the directory is skipped until it carries a
# CMakeLists.txt.
# ---------------------------------------------------------------------------
if (GE_BUILD_SAMPLES
    AND GE_ENABLE_MEDIA
    AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/CMakeLists.txt")
  add_subdirectory (samples)
endif ()

# ---------------------------------------------------------------------------
# Install & export (13 §1: the public surface is include/ge/{c,cpp,media,infer}).
#   find_package(ge CONFIG)  ->  ge::ge_core / ge::ge_infer / ge::ge_media / ge::ge_engine
#   pkg-config               ->  ge.pc (core+infer), ge-media.pc (media, Requires: ge)
# ---------------------------------------------------------------------------
install (
  DIRECTORY include/ge
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.h")

set (GE_INSTALL_TARGETS ge_build_flags ge_headers ge_core ge_infer ge_engine)
if (GE_ENABLE_MEDIA)
  list (APPEND GE_INSTALL_TARGETS ge_media)
endif ()
install (
  TARGETS ${GE_INSTALL_TARGETS}
  EXPORT geTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install (
  EXPORT geTargets
  NAMESPACE ge::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ge)

include (CMakePackageConfigHelpers)
set (GE_CONFIG_HAS_MEDIA ${GE_ENABLE_MEDIA})
set (GE_CONFIG_HAS_ONNX ${GE_ENABLE_ONNX})
configure_package_config_file (cmake/geConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/geConfig.cmake"
                               INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ge)
write_basic_package_version_file ("${CMAKE_CURRENT_BINARY_DIR}/geConfigVersion.cmake" COMPATIBILITY SameMajorVersion)
install (FILES "${CMAKE_CURRENT_BINARY_DIR}/geConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/geConfigVersion.cmake"
         DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ge)

# pkg-config. Static libraries: the consumer link line carries the private
# system deps; FFmpeg/ORT come in through Requires.private.
set (GE_PC_CORE_REQUIRES_PRIVATE "")
if (GE_ENABLE_ONNX)
  set (GE_PC_CORE_REQUIRES_PRIVATE "libonnxruntime")
endif ()
set (GE_PC_DL "")
if (CMAKE_DL_LIBS)
  set (GE_PC_DL " -l${CMAKE_DL_LIBS}")
endif ()
configure_file (cmake/ge.pc.in "${CMAKE_CURRENT_BINARY_DIR}/${GE_PACKAGE_NAME}.pc" @ONLY)
install (FILES "${CMAKE_CURRENT_BINARY_DIR}/${GE_PACKAGE_NAME}.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
if (GE_ENABLE_MEDIA)
  configure_file (cmake/ge-media.pc.in "${CMAKE_CURRENT_BINARY_DIR}/${GE_MEDIA_PACKAGE_NAME}.pc" @ONLY)
  install (FILES "${CMAKE_CURRENT_BINARY_DIR}/${GE_MEDIA_PACKAGE_NAME}.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif ()
