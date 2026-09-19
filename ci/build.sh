#!/bin/sh
# One CI lane: configure + build + ctest. Shared by .github/workflows/ci.yml
# and local container runs (see ci/Dockerfile.ci).
#
#   sh ci/build.sh <release|asan|tsan> [regular|stress]
#
# regular (default): ctest -LE stress      -- every PR
# stress:            ctest -L  stress      -- nightly (docs/15 §12.1)
#
# Environment:
#   CC / CXX          compiler pair (default: gcc/g++ on Linux, cc/c++ elsewhere)
#   GE_JOBS           parallel build jobs (default: nproc)
#   GE_GENERATOR      CMake generator (default: Ninja if available, else Unix Makefiles)
#   GE_STRESS_*       forwarded to CMake cache (PARALLEL/REPEAT/ROUNDS/TIMEOUT)
set -eu

lane="${1:-release}"
suite="${2:-regular}"
root="$(cd "$(dirname "$0")/.." && pwd)"
jobs="${GE_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

case "$lane" in
  release) flags="-DCMAKE_BUILD_TYPE=Release" ;;
  asan)    flags="-DCMAKE_BUILD_TYPE=Debug -DGE_ENABLE_SANITIZERS=ON" ;;
  tsan)    flags="-DCMAKE_BUILD_TYPE=Debug -DGE_ENABLE_TSAN=ON" ;;
  *) echo "unknown lane '$lane' (release|asan|tsan)" >&2; exit 2 ;;
esac

stress_flags=""
for v in PARALLEL REPEAT ROUNDS TIMEOUT; do
  eval "val=\${GE_STRESS_$v:-}"
  [ -n "$val" ] && stress_flags="$stress_flags -DGE_STRESS_$v=$val"
done

generator="${GE_GENERATOR:-}"
if [ -z "$generator" ]; then
  if command -v ninja > /dev/null 2>&1; then generator="Ninja"; else generator="Unix Makefiles"; fi
fi

build="$root/build/ci-$lane"
# shellcheck disable=SC2086
cmake -S "$root" -B "$build" -G "$generator" $flags $stress_flags -DGE_ENABLE_MEDIA=ON -DGE_ENABLE_ONNX=ON
cmake --build "$build" -j "$jobs"

cd "$build"
case "$suite" in
  regular) ctest --output-on-failure -LE stress -j 4 ;;
  stress)  ctest --output-on-failure -L stress ;;
  *) echo "unknown suite '$suite' (regular|stress)" >&2; exit 2 ;;
esac
