#!/bin/bash
# Builds inject.dex (multi-touch test injector) into ~/ftl-build/inject/
set -e
SDK=${ANDROID_SDK:-$HOME/android-sdk}
BT=$(ls -d $SDK/build-tools/* | sort -V | tail -1)
OUT=$HOME/ftl-build/inject
mkdir -p $OUT/classes
PLAT=$(ls -d $SDK/platforms/android-* | sort -V | tail -1)
javac --release 11 -cp $PLAT/android.jar -d $OUT/classes "$(dirname "$0")/Inject.java"
$BT/d8 --min-api 26 --output $OUT $OUT/classes/*.class
mv $OUT/classes.dex $OUT/inject.dex
echo "$OUT/inject.dex"
