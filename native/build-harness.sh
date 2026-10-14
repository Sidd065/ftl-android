#!/bin/bash
# Build the aarch64 Linux test harness (run with qemu-aarch64 -L ~/arm64-root).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SYSROOT=${SYSROOT:-$HOME/arm64-root}
BUILD=${BUILD:-$HOME/ftl-build/harness}
cmake -G Ninja -S "$HERE" -B "$BUILD" -DFTL_HARNESS=ON \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=clang -DCMAKE_ASM_COMPILER=clang \
  -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu -DCMAKE_ASM_COMPILER_TARGET=aarch64-linux-gnu \
  -DCMAKE_SYSROOT="$SYSROOT" -DCMAKE_BUILD_TYPE=Debug \
  -DLIBCXX_DIR="$HOME/ftl-build/libcxx/linux" >/dev/null
ninja -C "$BUILD"
echo "built $BUILD/ftlrun"
