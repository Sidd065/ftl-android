#!/bin/bash
# Builds libc++/libc++abi/libunwind (LLVM 19) with Apple arm64 ABI settings
# for loading iOS binaries: alternate std::string layout, non-unique RTTI bit,
# Darwin ctype masks, and libunwind able to walk Mach-O compact-unwind frames.
#   ./build.sh android   -> out/android/lib/*.a
#   ./build.sh linux     -> out/linux/lib/*.a   (aarch64 glibc, qemu test harness)
set -euo pipefail
TARGET=${1:-android}
HERE=$(cd "$(dirname "$0")" && pwd)
LLVM_SRC=${LLVM_SRC:-$HOME/src/llvm-project-19.1.7.src}
NDK=${NDK:-$HOME/android-sdk/ndk/28.2.13676358}
OUT=${LIBCXX_OUT:-$HOME/ftl-build/libcxx}/$TARGET
BUILD=${LIBCXX_BUILD:-$HOME/ftl-build/libcxx-build}-$TARGET
ABI_DEFINES="_LIBCPP_ABI_ALTERNATE_STRING_LAYOUT"
# -fc++-abi=arm: constructors/destructors return `this` like Apple arm64 (callers rely on it)
EXTRA_CXX="-D_LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION=3 -D_LIBCPP_DARWIN_CTYPE_ABI -fc++-abi=arm"
# Apply our source changes (Darwin ctype masks, regex, Mach-O unwind hook) once.
if patch -d "$LLVM_SRC" -p1 -R --dry-run -s -f < "$HERE/llvm-19.1.7-ftl.patch" >/dev/null 2>&1; then
  echo "LLVM sources already patched"
else
  patch -d "$LLVM_SRC" -p1 -N < "$HERE/llvm-19.1.7-ftl.patch"
fi
COMMON=(
  -G Ninja -S "$LLVM_SRC/runtimes" -B "$BUILD"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="$OUT"
  -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx"
  -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_SHARED=OFF
  -DLIBCXX_ENABLE_STATIC=ON -DLIBCXXABI_ENABLE_STATIC=ON -DLIBUNWIND_ENABLE_STATIC=ON
  -DLIBCXX_ABI_VERSION=1 -DLIBCXX_ABI_NAMESPACE=__1
  -DLIBCXX_ABI_DEFINES="$ABI_DEFINES"
  -DLIBCXX_CXX_ABI=libcxxabi
  -DLIBCXXABI_USE_LLVM_UNWINDER=ON
  -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON
  -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON
  -DLIBCXX_HERMETIC_STATIC_LIBRARY=ON -DLIBCXXABI_HERMETIC_STATIC_LIBRARY=ON
  -DLIBUNWIND_HIDE_SYMBOLS=ON
  -DLIBUNWIND_ADDITIONAL_COMPILE_FLAGS="-D_LIBUNWIND_FTL_MACHO"
  -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF
  -DLIBUNWIND_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_DOCS=OFF -DLIBUNWIND_INCLUDE_DOCS=OFF
  -DLIBCXX_ENABLE_LOCALIZATION=ON -DLIBCXX_ENABLE_WIDE_CHARACTERS=ON
  -DLIBCXX_ENABLE_EXCEPTIONS=ON -DLIBCXXABI_ENABLE_EXCEPTIONS=ON
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXXABI_USE_COMPILER_RT=ON -DLIBUNWIND_USE_COMPILER_RT=ON
)
if [ "$TARGET" = android ]; then
  cmake "${COMMON[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=none \
    -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer $EXTRA_CXX" -DCMAKE_C_FLAGS="-fno-omit-frame-pointer"
else
  SYSROOT=$HOME/arm64-root
  cmake "${COMMON[@]}" \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu -DCMAKE_CXX_COMPILER_TARGET=aarch64-linux-gnu \
    -DCMAKE_ASM_COMPILER_TARGET=aarch64-linux-gnu \
    -DCMAKE_SYSROOT="$SYSROOT" \
    -DCMAKE_C_FLAGS="-fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE $EXTRA_CXX" \
    -DLIBCXX_USE_COMPILER_RT=OFF -DLIBCXXABI_USE_COMPILER_RT=OFF -DLIBUNWIND_USE_COMPILER_RT=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DPython3_EXECUTABLE=/usr/bin/python3 -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
fi
ninja -C "$BUILD" cxx cxxabi unwind
ninja -C "$BUILD" install-cxx install-cxxabi install-unwind
echo "built into $OUT"
