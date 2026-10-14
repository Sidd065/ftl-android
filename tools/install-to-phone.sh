#!/bin/bash
# Installs the FTL port on a phone connected with USB (or wireless) debugging and,
# given your copy of the iPad game, copies the two game files to where the app
# imports them automatically on first launch.
#
#   tools/install-to-phone.sh [GAME]
#     GAME: an extracted .ipa folder (containing Payload/FTL.app), an FTL.app
#           folder, or a .ipa/.zip file
#   env:  APK (default ~/ftl-build/apk/ftlport.apk), ADB (default adb),
#         ANDROID_SERIAL to pick a device when several are connected
set -euo pipefail
ADB=${ADB:-adb}
APK=${APK:-$HOME/ftl-build/apk/ftlport.apk}
PKG=dev.ftlport
GAME=${1:-}

[ -f "$APK" ] || { echo "APK not found: $APK (build it with android/build-apk.sh)"; exit 1; }
"$ADB" get-state >/dev/null || { echo "No device. Enable USB debugging and check 'adb devices'."; exit 1; }
abi=$("$ADB" shell getprop ro.product.cpu.abilist | tr -d '\r')
case "$abi" in *arm64-v8a*) ;; *) echo "This device ($abi) can't run the FTL port: it needs a 64-bit ARM (arm64-v8a) CPU."; exit 1 ;; esac

echo "== installing $APK"
"$ADB" install -r "$APK"

if [ -z "$GAME" ]; then
  echo "Done. Open FTL on the phone and pick your .ipa/.zip or extracted folder."
  exit 0
fi

if [ -d "$GAME" ]; then
  dat=$(find "$GAME" -maxdepth 4 -name ftl.dat -print -quit)
  [ -n "$dat" ] && [ -f "$(dirname "$dat")/FTL" ] || { echo "No FTL + ftl.dat found under $GAME"; exit 1; }
  app=$(dirname "$dat")
  # Starting the app once creates its storage folder with the right owner.
  "$ADB" shell am start -W -n $PKG/.GameActivity >/dev/null
  sleep 2
  "$ADB" shell am force-stop $PKG
  dest=/sdcard/Android/data/$PKG/files
  echo "== copying game files from $app"
  if "$ADB" push "$app/FTL" "$app/ftl.dat" "$dest/"; then
    echo "== starting FTL (it imports the files on launch; the first start takes a little while)"
    "$ADB" shell am start -n $PKG/.GameActivity >/dev/null
    exit 0
  fi
  echo "Couldn't copy into the app's folder on this Android version; zipping and using Downloads instead."
  tmp=$(mktemp -d)
  (cd "$app/.." && zip -q -0 -r "$tmp/ftl-ipad.zip" "$(basename "$app")/FTL" "$(basename "$app")/ftl.dat")
  GAME="$tmp/ftl-ipad.zip"
fi

echo "== copying $(basename "$GAME") to the phone's Download folder"
"$ADB" push "$GAME" /sdcard/Download/
"$ADB" shell am start -n $PKG/.GameActivity >/dev/null
echo "Done. In FTL, tap 'Select .ipa / .zip file' and choose $(basename "$GAME") in Downloads."
