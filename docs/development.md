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
- `HYPERSPACE=1` adds Hyperspace, built from source (see below);
  `HYPERSPACE_SO=/path/to/libHyperspace.so` adds a build of it you already have.
- `RELEASE=1` makes a release, which looks for updates (see below).

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
pause action, hotkey lookup, menu and window state, the PNG reader and the
ftl.dat reader for mods). They're only enabled when the binary's LC_UUID matches
that build, and Hyperspace only starts on it. Other versions run without them:
the app tells the player once, refuses mods that need Hyperspace, and shows the
version (from Info.plist) in settings and the build in the logs.

## Mods

Mods are installed with [ftlman](https://github.com/afishhh/ftlman), the same
way as on PC. `ftlman/build.sh` builds it for Android from release 0.7.4 with
`ftlman/headless.patch`, which makes its window optional, and the APK carries
it as `libftlman.so`. When the player applies mods, the app runs
`ftlman patch` on a copy of the original `ftl.dat` (kept as `ftl.dat.vanilla`)
and swaps the result in; if anything fails, nothing changes. ftlman is
GPL-2.0 (see `ftlman/LICENSE`), and each release has its source attached.

The app keeps a copy of each mod in `files/mods`, listed in order in
`files/mods/list.json`. A copy comes from a file the player added or opened
with FTL, or from the mods folder: a folder the player picked
(`ACTION_OPEN_DOCUMENT_TREE`, kept with a persisted read permission). Each time
the Mods screen opens, and before applying, `Mods.syncFolder` lists the
folder's `.ftl` and `.zip` files and copies in the new ones and the ones whose
size or modification time changed. Mods whose files are gone leave the list,
and a file that isn't a mod stays in it with the reason. A copy replaced since
mods were last applied is marked, so the screen offers to apply again.

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
hooking the game's functions in memory; big mods like Multiverse need it. The
iPad game's Hyperspace, for this app and the other ports of the iPad game
(Linux, Switch), is the `iOS-binary-ports` branch of
[FTL-Hyperspace-Dino](https://github.com/The-Dumb-Dino/FTL-Hyperspace-Dino/tree/iOS-binary-ports)
(`buildscripts/android/` in it explains how to build it and what works).
`hyperspace/build-android.sh` builds the commit pinned in `hyperspace/source.sh`
(`HS_REPO` and `HS_REF` pick another) and packs its source as
`FTL-Hyperspace-<commit>-source.tar.gz` next to the library, for releases to
attach. Besides the app's tools it needs swig and Lua 5.3 with lpeg and lfs.
An APK built with `HYPERSPACE=1` (or `HYPERSPACE_SO`) carries it as
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

## Releases and the updater

`RELEASE=1 android/build-apk.sh` builds a release (`versionName` in the manifest
must be X.Y.Z). Only releases have the updater: at start, while "Look for
updates" is on in Settings, the app asks GitHub for the latest release of
[Sidd065/ftl-android](https://github.com/Sidd065/ftl-android/releases)
(`UPDATE_REPO_OVERRIDE=owner/repo` for another). When its tag is newer than the
app's version, the player can update, wait, or skip that version. The app
downloads the release's `.apk` (the first asset whose name ends in `.apk`),
checks its size and SHA-256 against what GitHub lists, and hands it to
Android's package installer (`Updater.java`). Android asks the player to
confirm, and the first time to allow FTL to install apps; it only installs an
APK signed with the same key as the installed app.

So a release needs a tag `vX.Y.Z` matching `versionName`, published as a
normal release (drafts and pre-releases aren't offered), with one `.apk`
attached. A release APK asks for two more permissions, the network and
installing apps; builds without `RELEASE=1` ask for neither and never contact
GitHub.

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
