#!/bin/bash
# Builds Hyperspace for the app: libHyperspace.so for FTL for iPad on Android.
#   hyperspace/build-android.sh   -> ~/ftl-build/hyperspace-android/libHyperspace.so
#                                    and FTL-Hyperspace-<commit>-source.tar.gz next to it
# The iPad game's Hyperspace (the fork and commit in source.sh) with its Android
# target, buildscripts/android/ in it. HS_REPO and HS_REF pick another clone,
# branch or commit. android/build-apk.sh runs this with HYPERSPACE=1.
# Needs: libcxx/build.sh android, the NDK, cmake, ninja, swig, Lua 5.3 with lpeg
# and lfs (apt install swig lua5.3 lua-lpeg lua-filesystem), git, curl, zip, tar.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=${ANDROID_HOME:-$HOME/android-sdk}
export ANDROID_NDK_HOME=${NDK:-$SDK/ndk/28.2.13676358}
export FTL_LIBCXX=${LIBCXX_DIR:-$HOME/ftl-build/libcxx/android}
SRC=${HS_SRC:-$HOME/ftl-build/hyperspace-src}
OUT=${HS_OUT:-$HOME/ftl-build/hyperspace-android}
. "$HERE/source.sh"
BUILD_TYPE=${1:-Release}

[ -f "$FTL_LIBCXX/lib/libc++.a" ] || { echo "run libcxx/build.sh android first" >&2; exit 1; }
hs_checkout
if [ ! -x "$SRC/buildscripts/android/build.sh" ]; then
  echo "$REPO ($REF) has no Android target (buildscripts/android/): use HS_REPO/HS_REF" >&2
  exit 1
fi

"$SRC/buildscripts/android/setup.sh" >/dev/null
"$SRC/buildscripts/android/build.sh" "$BUILD_TYPE"

mkdir -p "$OUT"
BUILD=$SRC/build-android-$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')
cp "$BUILD/libHyperspace.so" "$OUT/libHyperspace.so"
# The source it was built from, for releases that carry it (GPL)
COMMIT=$(git -C "$SRC" rev-parse --short=8 HEAD)
rm -f "$OUT"/FTL-Hyperspace-*-source.tar.gz
git -C "$SRC" archive --format=tar.gz --prefix="FTL-Hyperspace-$COMMIT/" \
  -o "$OUT/FTL-Hyperspace-$COMMIT-source.tar.gz" HEAD
ls -la "$OUT"
