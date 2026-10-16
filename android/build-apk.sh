#!/bin/bash
# Builds the FTL Port APK (no Gradle): native lib via CMake/NDK, then aapt2,
# javac, d8, zipalign (16 KB page aligned .so) and apksigner.
#   ./build-apk.sh [release|debug]   -> $OUT/ftlport.apk
#   GAME_APP=/path/to/FTL.app ./build-apk.sh   -> $OUT/ftl-personal.apk with the game's own icon
#   DEBUGGABLE=1 ./build-apk.sh   -> marks the APK debuggable (adb run-as can reach saves; code stays optimized)
#     (taken from your copy at build time; never stored in the repo or the default APK)
# ftlman, which installs mods, is built separately with ftlman/build.sh (FTLMAN=path to override).
#   HYPERSPACE_SO=/path/to/libHyperspace.so ./build-apk.sh   -> also carries Hyperspace, which a
#     setting (off by default) loads; built from FTL-Hyperspace's android-arm64 branch
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SDK=${ANDROID_HOME:-$HOME/android-sdk}
NDK=${NDK:-$SDK/ndk/28.2.13676358}
BT=$SDK/build-tools/36.0.0
PLATFORM=$SDK/platforms/android-36/android.jar
OUT=${OUT:-$HOME/ftl-build/apk}
NATIVE_BUILD=${NATIVE_BUILD:-$HOME/ftl-build/android-native}
LIBCXX_DIR=${LIBCXX_DIR:-$HOME/ftl-build/libcxx/android}
KEYSTORE=${KEYSTORE:-$HOME/ftl-build/keys/ftlport.keystore}
FTLMAN=${FTLMAN:-$HOME/ftl-build/ftlman/libftlman.so}
BUILD_TYPE=${1:-release}
# A build with the game's icon is for your own device only; keep it apart.
APK_NAME=$([ -n "${GAME_APP:-}" ] && echo ftl-personal.apk || echo ftlport.apk)

if [ ! -f "$FTLMAN" ]; then
  echo "ftlman not found at $FTLMAN: run ftlman/build.sh first" >&2
  exit 1
fi
mkdir -p "$OUT" "$(dirname "$KEYSTORE")"
WORK=$(mktemp -d "$OUT/work.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

echo "== native"
cmake -G Ninja -S "$ROOT/native" -B "$NATIVE_BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=none \
  -DCMAKE_BUILD_TYPE=$([ "$BUILD_TYPE" = debug ] && echo Debug || echo RelWithDebInfo) \
  -DLIBCXX_DIR="$LIBCXX_DIR" >/dev/null
ninja -C "$NATIVE_BUILD"
mkdir -p "$WORK/lib/arm64-v8a"
cp "$FTLMAN" "$WORK/lib/arm64-v8a/libftlman.so"
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-debug -o "$WORK/lib/arm64-v8a/libftlport.so" \
  "$NATIVE_BUILD/libftlport.so"
LIBS=(lib/arm64-v8a/libftlman.so)
if [ -n "${HYPERSPACE_SO:-}" ]; then
  "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-debug -o "$WORK/lib/arm64-v8a/libHyperspace.so" \
    "$HYPERSPACE_SO"
  LIBS+=(lib/arm64-v8a/libHyperspace.so)
fi

echo "== resources"
"$BT/aapt2" compile --dir "$HERE/res" -o "$WORK/res.zip"
OVERLAY=()
[ "${DEBUGGABLE:-0}" = 1 ] && OVERLAY+=(--debug-mode)
if [ -n "${GAME_APP:-}" ]; then
  python3 -I "$ROOT/tools/game_icon.py" "$GAME_APP" "$WORK/res-game"
  "$BT/aapt2" compile --dir "$WORK/res-game" -o "$WORK/res-game.zip"
  OVERLAY+=(--auto-add-overlay -R "$WORK/res-game.zip")
fi
mkdir -p "$WORK/gen"
"$BT/aapt2" link -o "$WORK/base.apk" -I "$PLATFORM" --manifest "$HERE/AndroidManifest.xml" \
  --min-sdk-version 26 --target-sdk-version 35 --java "$WORK/gen" "$WORK/res.zip" "${OVERLAY[@]}"

echo "== java"
mkdir -p "$WORK/classes"
javac --release 17 -nowarn -cp "$PLATFORM" -d "$WORK/classes" \
  $(find "$HERE/src" "$WORK/gen" -name '*.java')
"$BT/d8" --min-api 26 $([ "$BUILD_TYPE" = debug ] && echo --debug || echo --release) --lib "$PLATFORM" \
  --output "$WORK" $(find "$WORK/classes" -name '*.class')

echo "== package"
cp "$WORK/base.apk" "$WORK/unaligned.apk"
(cd "$WORK" && zip -q -X "$WORK/unaligned.apk" classes.dex "${LIBS[@]}" \
  && zip -q -X -0 "$WORK/unaligned.apk" lib/arm64-v8a/libftlport.so)
"$BT/zipalign" -P 16 -f 4 "$WORK/unaligned.apk" "$WORK/aligned.apk"
if [ ! -f "$KEYSTORE" ]; then
  keytool -genkeypair -keystore "$KEYSTORE" -storepass ftlport -keypass ftlport -alias ftlport \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=FTL Port, O=Personal" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:ftlport --key-pass pass:ftlport \
  --out "$OUT/$APK_NAME" "$WORK/aligned.apk"
"$BT/apksigner" verify "$OUT/$APK_NAME"
ls -la "$OUT/$APK_NAME"
