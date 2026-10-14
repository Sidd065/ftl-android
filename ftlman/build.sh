#!/bin/bash
# Builds ftlman, the mod manager the app uses to install mods, for Android.
#   ftlman/build.sh   -> ~/ftl-build/ftlman/libftlman.so
# ftlman (https://github.com/afishhh/ftlman, GPL-2.0) is built from a fixed
# release with headless.patch applied, which makes its window optional so it
# can be built as a command-line tool. The app runs it from its native library
# folder, hence the lib*.so name. Needs a nightly Rust toolchain with the
# aarch64-linux-android target (rustup target add aarch64-linux-android).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${ANDROID_HOME:-$HOME/android-sdk}
NDK=${NDK:-$SDK/ndk/28.2.13676358}
SRC=${FTLMAN_SRC:-$HOME/ftl-build/ftlman-src}
OUT=${OUT:-$HOME/ftl-build/ftlman}
REPO=https://github.com/afishhh/ftlman
COMMIT=7ad94795f9eff7f4bf005ea40d9bb2cf4d57ec88  # v0.7.4
CARGO=${CARGO:-$(command -v cargo || echo "$HOME/.cargo/bin/cargo")}

if [ ! -d "$SRC/.git" ]; then
  git clone "$REPO" "$SRC"
fi
git -C "$SRC" fetch -q origin
git -C "$SRC" checkout -q -f "$COMMIT"
git -C "$SRC" apply "$HERE/headless.patch"

BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$BIN/aarch64-linux-android26-clang
export CC_aarch64_linux_android=$BIN/aarch64-linux-android26-clang
export CXX_aarch64_linux_android=$BIN/aarch64-linux-android26-clang++
export AR_aarch64_linux_android=$BIN/llvm-ar
(cd "$SRC" && "$CARGO" build --locked --release --no-default-features --target aarch64-linux-android)

mkdir -p "$OUT"
cp "$SRC/target/aarch64-linux-android/release/ftlman" "$OUT/libftlman.so"
ls -la "$OUT/libftlman.so"
