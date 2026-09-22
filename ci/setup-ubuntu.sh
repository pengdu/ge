#!/bin/sh
# Installs the Linux build dependencies (Ubuntu 24.04). Used verbatim by
# ci/Dockerfile.ci and by .github/workflows/ci.yml on the ubuntu-24.04 runner.
#
# Ubuntu 24.04 carries FFmpeg 6.1 (libavcodec 60.31), so the Linux lanes
# exercise the FFmpeg 6.x branch of src/media/ffmpeg.h while macOS covers 9.x.
# ONNX Runtime comes from the official CPU tarball plus a hand-written
# pkg-config file so CMake finds it the same way it finds Homebrew's package.
set -eu

ORT_VERSION="${ORT_VERSION:-1.30.0}"
ORT_PREFIX="${ORT_PREFIX:-/opt/onnxruntime}"
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update
$SUDO apt-get install -y --no-install-recommends \
  ca-certificates curl git pkg-config make ninja-build cmake \
  gcc g++ clang \
  libgtest-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libavfilter-dev \
  ffmpeg fonts-dejavu-core

arch="$(uname -m)"
case "$arch" in
  x86_64)  ort_arch=x64 ;;
  aarch64) ort_arch=aarch64 ;;
  *) echo "unsupported arch $arch" >&2; exit 1 ;;
esac

if [ ! -f "$ORT_PREFIX/lib/pkgconfig/libonnxruntime.pc" ]; then
  tmp="$(mktemp -d)"
  curl -fsSL -o "$tmp/ort.tgz" \
    "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-${ort_arch}-${ORT_VERSION}.tgz"
  $SUDO mkdir -p "$ORT_PREFIX"
  $SUDO tar -xzf "$tmp/ort.tgz" -C "$ORT_PREFIX" --strip-components=1
  rm -rf "$tmp"
  $SUDO mkdir -p "$ORT_PREFIX/lib/pkgconfig"
  printf '%s\n' \
    "prefix=$ORT_PREFIX" \
    'libdir=${prefix}/lib' \
    'includedir=${prefix}/include' \
    '' \
    'Name: onnxruntime' \
    'Description: ONNX runtime' \
    "Version: ${ORT_VERSION}" \
    'Libs: -L${libdir} -lonnxruntime' \
    'Cflags: -I${includedir}' \
    | $SUDO tee "$ORT_PREFIX/lib/pkgconfig/libonnxruntime.pc" > /dev/null
  echo "$ORT_PREFIX/lib" | $SUDO tee /etc/ld.so.conf.d/onnxruntime.conf > /dev/null
  $SUDO ldconfig
fi

echo "PKG_CONFIG_PATH=$ORT_PREFIX/lib/pkgconfig"
