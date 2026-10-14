# ftl-android

Play the iPad version of FTL: Faster Than Light on an Android phone.

This isn't a remake. The original arm64 iOS executable runs inside an Android
app: a small Mach-O loader maps it into memory, and just enough of iOS is
reimplemented underneath it (an Objective-C runtime, libSystem, Foundation,
UIKit, EAGL, AudioUnit) for the game to run. On top of that sits a touch layer
that makes the iPad interface usable on a phone-sized screen.

No game files are included. You need your own copy of FTL for iPad; on first
launch the app imports two files from it (`FTL` and `ftl.dat`) into its
private storage.

![A fight on a 20:9 phone](docs/combat.png)

| Hold a finger to magnify small buttons | Pinch to zoom; the minimap shows where you are |
|---|---|
| ![Magnifier](docs/magnifier.png) | ![Pinch zoom with minimap](docs/zoom.png) |

## Requirements

- An arm64 Android device on Android 8.0 or newer, with OpenGL ES 3.0.
- FTL for iPad 1.6.21 with an unencrypted executable (`cryptid 0`). Copies
  downloaded from the App Store are FairPlay-encrypted and can't run outside
  iOS; the importer checks for this. Other versions of the game are untested
  and would run without the version-specific extras (two-finger pause,
  Back/quit handling, keyboard shortcuts).

## Building

You need the Android SDK (platform 36, build-tools 36.0.0), NDK 28.2.13676358,
JDK 17 or newer, CMake, Ninja, and the LLVM 19.1.7 sources.

    libcxx/build.sh android     # libc++/libc++abi/libunwind with Apple's arm64 ABI
    android/build-apk.sh        # -> ~/ftl-build/apk/ftlport.apk

The game was built against Apple's libc++, so the port links its own copy built
to match (alternate string layout, constructors that return `this`, Darwin
ctype masks, and libunwind that can read Mach-O compact unwind info). The
changes to LLVM are in `libcxx/llvm-19.1.7-ftl.patch`; `build.sh` applies them.

There's no Gradle: the native library is built with CMake and the NDK, then the
APK is put together with aapt2, javac, d8, zipalign and apksigner. A signing key
is generated in `~/ftl-build/keys/` the first time.

Two optional build variables:

- `GAME_APP=/path/to/FTL.app` uses the game's own icon, taken from your copy
  at build time, and writes `ftl-personal.apk` instead. That APK contains the
  game's artwork, so keep it to yourself.
- `DEBUGGABLE=1` marks the APK debuggable, so `adb shell run-as dev.ftlport`
  can reach the save files (handy for backups).

## Installing

With the phone connected and USB debugging on:

    tools/install-to-phone.sh /path/to/FTL_1.6.21_ios_8.0

This installs the APK, copies the two game files into the app's folder and
starts it. The app imports them on launch and deletes the copies.

To do it by hand, install the APK and pick your `.ipa` from the import screen.
A zip of `FTL.app`, a zip of just `FTL` and `ftl.dat`, or the extracted folder
also work.

## Playing on a phone

The iPad interface is drawn across the whole screen. On a 6.5" phone that makes
everything about half its iPad size, so there are a few additions:

| Gesture | What it does |
|---|---|
| Tap | Same as on the iPad. Buttons trigger when you lift your finger. |
| Hold | Shows a magnifier: a 2x view of what's under your finger, with the touch point marked. Slide onto the target and lift. |
| Pinch / two-finger drag | Zoom up to 3x and pan. A minimap shows where you are; tap it or pinch out to zoom back. |
| Two-finger tap | Pause and resume. |
| Back | Closes the open window or opens the game menu. On the title screen it asks whether to quit; the game saves first. |

The iOS build has no keyboard shortcuts of its own, so desktop-style ones are
added and switch on the first time you press a key: Space pauses, 1-8 power
weapons and drones (Shift removes power), the letter keys power systems, J
jumps, U/C/I open upgrades, crew and cargo, Z/X open or close all doors, V
toggles autofire, Esc opens the menu.

A mouse works too: the left button acts as a finger, the right button is Back,
and the wheel zooms.

The game ignores controllers, so a controller drives an on-screen cursor
instead. The left stick moves it (the d-pad for fine moves), A taps, holds or
drags at the cursor, B is Back, X pauses, LB/RB zoom, the right stick pans
while zoomed in, holding LT shows the magnifier, Start opens the menu and
Select resets the zoom.

Long-press the app icon for settings: magnifier (automatic on phone-sized
screens, always or off), two-finger pause, minimap, render resolution, and
re-importing the game files.

The game saves whenever you switch away from it, like on iOS.

## How it works

```
GameActivity.java    SurfaceView, input, dialogs, lifecycle
  | JNI
plat_android.c       EGL surface hand-off, event queue, AAudio
ux/                  layout, zoom, gestures, magnifier, minimap, hotkeys
fw/                  UIKit, Foundation, QuartzCore/EAGL, AudioUnit, GameKit stubs;
                     EAGL maps onto GLES 3 (the game renders to an FBO that gets
                     blitted to the window), BGRA swizzling, PVRTC decoded on the CPU
objc/                Objective-C runtime with Apple's ABI (objc_msgSend in assembly)
darwin/              libSystem: Darwin structs, pthreads, Mach semaphores, errno
loader/              Mach-O loader: maps the arm64 slice, rebases, binds imports
```

The game believes it's running on an iPad whose screen has the phone's aspect
ratio. It lays out a canvas 768 units tall and as wide as the screen, rendered
at 1536 pixels tall on most phones so text stays sharp when scaled or zoomed.

A few features hook specific addresses in the 1.6.21 binary (the logger, the
pause action, hotkey lookup and menu state). They're only enabled when the
binary's LC_UUID matches that build.

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
two seconds and holds 60 fps. Over a 12-minute session of combat, jumps and a
store visit, memory stayed around 176 MB and the phone didn't throttle. The
app logs frame pacing every 30 seconds (`adb logcat -s FTLPort`).

## Known issues

- Game Center is disabled, so achievements stay local.
- Text is small on phones. The magnifier and zoom help, but it's still the iPad
  layout.
- Only tested on Adreno so far (plus Android 11 in Docker). Mali and PowerVR
  GPUs are untested.

FTL: Faster Than Light, including the artwork in the screenshots, is made by
Subset Games. This project isn't affiliated with or endorsed by them.
