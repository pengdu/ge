# ge

`ge` is a single-process C++20 graph execution engine for media, inference, and custom data flows.

The control plane validates JSON graph specifications, negotiates edge capabilities, and atomically publishes immutable runtime topologies. The data plane streams packets through bounded channels with backpressure. The runtime supports live graph mutations, hot node-parameter updates, C-ABI plugins, FFmpeg media operators, optional ONNX Runtime inference, metrics export, and event notifications.

## Features

- Immutable graph topology publication with atomic live mutations
- Capability negotiation and validation for nodes and edges
- Packet-based scheduling with backpressure and asynchronous execution
- C and C++ public APIs, plus a stable C-ABI plugin interface
- FFmpeg 6+ media pipeline: demuxing, decoding, filtering, scaling, encoding, muxing, composition, mixing, and rendition control
- Optional ONNX Runtime inference operators
- Runtime events, audit records, resource accounting, and Prometheus text-format metrics
- Unit, integration, stress, soak, ABI, and install-consumer tests

## Requirements

- CMake 3.25 or newer
- A C11 compiler and a C++20 compiler
- Threads
- `pkg-config` / `pkgconf` when enabling optional dependencies
- FFmpeg 6.0 or newer to build `ge_media`
- ONNX Runtime to enable ONNX inference
- GoogleTest when building tests

When FFmpeg or ONNX Runtime cannot be discovered, CMake disables the related optional component and continues configuring the core runtime.

## Build

Configure and build the default configuration:

```sh
cmake -S . -B build
cmake --build build -j
```

The default build enables media operators, ONNX inference, samples, and tests when their dependencies are available.

### Build options

| Option | Default | Description |
| --- | --- | --- |
| `GE_ENABLE_MEDIA` | `ON` | Build FFmpeg media operators. |
| `GE_ENABLE_ONNX` | `ON` | Build ONNX Runtime inference support. |
| `GE_BUILD_SAMPLES` | `ON` | Build samples when media support is available. |
| `GE_ENABLE_SANITIZERS` | `OFF` | Enable AddressSanitizer and UndefinedBehaviorSanitizer. |
| `GE_ENABLE_TSAN` | `OFF` | Enable ThreadSanitizer; mutually exclusive with `GE_ENABLE_SANITIZERS`. |
| `BUILD_TESTING` | `ON` | Build and register the test suite. |

Examples:

```sh
cmake -S . -B build/core -DGE_ENABLE_MEDIA=OFF -DGE_ENABLE_ONNX=OFF
cmake --build build/core -j

cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DGE_ENABLE_SANITIZERS=ON
cmake --build build/asan -j
```

## Test

Run the regular test suite:

```sh
ctest --test-dir build --output-on-failure -LE "stress|soak"
```

The CI helper configures, builds, and runs a selected lane:

```sh
sh ci/build.sh release regular
sh ci/build.sh asan regular
sh ci/build.sh tsan regular
sh ci/build.sh release stress
GE_SOAK_DURATION=30m sh ci/build.sh release soak
```

## Install and consume

Install the package:

```sh
cmake --install build --prefix /usr/local
```

CMake consumers can use the exported targets:

```cmake
find_package(ge CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ge::ge_core ge::ge_infer)
```

When built and installed, media support is available as `ge::ge_media`. The shared C API library is `ge::ge_engine`.

`pkg-config` packages are also installed:

```sh
pkg-config --cflags --libs ge
pkg-config --cflags --libs ge-media
```

## Public interfaces

| Area | Headers |
| --- | --- |
| C API and plugin ABI | `include/ge/c/` |
| C++ runtime API | `include/ge/cpp/` |
| Inference operators | `include/ge/infer/` |
| Media operators | `include/ge/media/` |
| JSON schemas | `schema/v1/` |

## Repository layout

```text
include/ge/       Public C, C++, inference, and media headers
src/              Runtime, scheduling, plugin, inference, and media implementations
schema/v1/        JSON schemas for graph specs, patches, capabilities, and plugin manifests
samples/          Sample applications and plugins
tests/            Unit, ABI, install, stress, soak, and benchmark targets
ci/               CI container and build scripts
docs/             Design specifications and implementation notes
```

## Documentation

The design and implementation documentation is available in [`docs/`](docs/). The documents are currently maintained in Chinese.

## License

See [LICENSE](LICENSE).
