# Development

## Building

You need the Android SDK (platform 36, build-tools 36.0.0), NDK 28.2.13676358,
JDK 17 or newer, CMake, Ninja, the LLVM 19.1.7 sources, and a nightly Rust
toolchain with the `aarch64-linux-android` target (for ftlman).

    libcxx/build.sh android     # libc++/libc++abi/libunwind with Apple's arm64 ABI
    ftlman/build.sh             # the mod installer -> ~/ftl-build/ftlman/libftlman.so
    android/build-apk.sh        # -> ~/ftl-build/apk/ftlport.apk

The game was built against Apple's libc++, so the port links its own copy built
to match (alternate string layout, constructors that return `this`, Darwin
ctype masks, and libunwind that can read Mach-O compact unwind info). The
changes to LLVM are in `libcxx/llvm-19.1.7-ftl.patch`; `build.sh` applies them.

There's no Gradle: the native library is built with CMake and the NDK, then the
APK is put together with aapt2, javac, d8, zipalign and apksigner. A signing key
is generated in `~/ftl-build/keys/` the first time. An APK you build yourself is
signed with your own key, so Android won't install it over a release (or the
other way round) without uninstalling first.

Optional build variables:

- `GAME_APP=/path/to/FTL.app` uses the game's own icon, taken from your copy
  at build time, as the release APKs do, and writes `ftl-personal.apk`.
  Without it the app gets a plain icon, so the repo itself has no game art.
- `DEBUGGABLE=1` marks the APK debuggable, so `adb shell run-as dev.ftlport`
  can reach the save files.
- `HYPERSPACE_SO=/path/to/libHyperspace.so` adds Hyperspace (see below).

## Installing from a computer

With USB debugging on, `adb install <apk>` installs the app; then import the
game on the phone. Or let the script do both: it installs the APK and copies
`FTL` and `ftl.dat` into the app's folder, and the app imports them when it
starts.

    APK=ftl-android-0.3.2.apk tools/install-to-phone.sh /path/to/FTL_1.6.21_ios_8.0

## How it works

```
GameActivity.java    SurfaceView, input, dialogs, lifecycle
  | JNI
plat_android.c       EGL surface hand-off, event queue, AAudio
ux/                  layout, zoom, gestures, magnifier, minimap, hotkeys, mods, PNG
fw/                  UIKit, Foundation, QuartzCore/EAGL, AudioUnit, GameKit stubs;
                     EAGL maps onto GLES 3 (the game renders to an FBO that gets
                     blitted to the window), BGRA swizzling, PVRTC decoded on the CPU
objc/                Objective-C runtime with Apple's ABI (objc_msgSend in assembly)
darwin/              libSystem: Darwin structs, pthreads, Mach semaphores, errno
loader/              Mach-O loader: maps the arm64 slice, rebases, binds imports
```

The original arm64 iOS executable runs inside the app: a small Mach-O loader
maps it into memory, and just enough of iOS is reimplemented underneath it for
the game to run. The game believes it's on an iPad whose screen has the phone's
aspect ratio. It lays out a canvas 768 units tall and as wide as the screen,
rendered at 1536 pixels tall on most phones so text stays sharp when zoomed.

A few features hook specific addresses in the 1.6.21 binary (the logger, the
pause action, hotkey lookup, menu and window state, and the ftl.dat reader for
mods). They're only enabled when the binary's LC_UUID matches that build. Other
versions of the game would run without them.

## Mods

Mods are installed with [ftlman](https://github.com/afishhh/ftlman), the same
way as on PC. `ftlman/build.sh` builds it for Android from release 0.7.4 with
`ftlman/headless.patch`, which makes its window optional, and the APK carries
it as `libftlman.so`. When the player applies mods, the app runs
`ftlman patch` on a copy of the original `ftl.dat` (kept as `ftl.dat.vanilla`)
and swaps the result in; if anything fails, nothing changes. ftlman is
GPL-2.0 (see `ftlman/LICENSE`), and each release has its source attached.

The iPad build needs some help with what mods add:

- It was built without its PNG reader, since its own art is all `.tex`
  textures. `ux/png.c` puts one back (stb_image).
- It looks for `x.tex` before `x.png`, so the ftl.dat reader hides an iPad
  texture when a mod added a PNG of the same name.
- It packs small images into atlases. The app writes the atlas lists without
  the images mods replaced into a small overlay that the reader serves instead.

ftlman reads mod text files as UTF-8, while Slipstream also accepts Windows-1252,
which some older mods were saved in. The app converts such files to UTF-8
before running ftlman, so those mods install as they do with Slipstream.

Mods for Hyperspace need an APK that carries Hyperspace (see below).

## Hyperspace

[Hyperspace](https://github.com/FTL-Hyperspace/FTL-Hyperspace) extends FTL by
hooking the game's functions in memory; big mods like Multiverse need it. Its
Android build is the `android-arm64` branch of FTL-Hyperspace, and each release
that carries it attaches the source it was built from (`buildscripts/android/`
in it explains how to build it and what works). An APK built with `HYPERSPACE_SO` carries it as
`libHyperspace.so`, and Settings gets a "Hyperspace (experimental)" switch,
off by default. When it's on, `hyperspace.c` loads the library once the game
is mapped, linked and protected, before any game code runs, and calls its
`hs_android_init` with where the game is (`hyperspace_host.h`, shared with
Hyperspace). Its logs (zhl.log, FTL_HS.log) go to the log folder with the
session logs. If it fails to start, the app says why and doesn't start the
game. For mods' right clicks, `ux.c` asks it whether a script listens for the
right button (`hs_android_wants_right_click`) and, if so, sends it the gesture
(one finger held, a tap with another) and a mouse's right button
(`hs_android_right_button`).

## Testing

`native/build-harness.sh` builds a desktop harness that runs the game under
`qemu-aarch64` with Mesa's surfaceless EGL (it needs `libcxx/build.sh linux`
and an arm64 glibc sysroot). It takes commands from a FIFO (taps, drags,
multi-finger gestures, screenshots, text, quit) and can record audio to a WAV
file. It's slow, but good for scripted checks.

For Android without a phone, `android/README-testing.md` describes running
Android 11 in Docker with ARM translation. `tools/inject/` is a small
multi-touch injector for testing pinch and two-finger gestures, since
`adb shell input` can only do one finger at a time.

On a Snapdragon phone (OPPO Find X9 Ultra, Android 16) the game loads in about
two seconds and holds 60 fps. Over a 12-minute session memory stayed around
176 MB and the phone didn't throttle. The app logs frame pacing every 30
seconds (`adb logcat -s FTLPort`).
