cmake_minimum_required(VERSION 3.20)
project(ftlport C ASM)

set(CMAKE_C_STANDARD 11)
set(LIBCXX_DIR "" CACHE PATH "Apple-ABI libc++ install dir (lib/ with libc++.a, libc++abi.a, libunwind.a)")
option(FTL_HARNESS "Build the Linux test harness executable instead of the Android library" OFF)

set(CORE_SOURCES
    src/loader/macho.c
    src/loader/symbols.c
    src/loader/symbols_cxx.c
    src/objc/objc_runtime.c
    src/objc/objc_api.c
    src/objc/objc_msgSend.S
    src/darwin/libsystem.c
    src/darwin/darwin_asm.S
    src/fw/foundation.c
    src/fw/uikit.c
    src/fw/quartz_eagl.c
    src/fw/present.c
    src/fw/pvrtc.c
    src/fw/gamecontroller.c
    src/fw/audio_misc.c
    src/port_main.c
    src/patches.c
    src/ux/ux.c
    src/ux/overlay.c
)

set(COMMON_FLAGS -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    -fvisibility=hidden -ffunction-sections -fdata-sections)

set(CXX_LIBS
    ${LIBCXX_DIR}/lib/libc++.a
    ${LIBCXX_DIR}/lib/libc++abi.a
    ${LIBCXX_DIR}/lib/libunwind.a)

if(FTL_HARNESS)
    add_executable(ftlrun ${CORE_SOURCES} src/platform/plat_linux.c src/debug/guard_alloc.c src/debug/debug_hooks.c)
    target_compile_definitions(ftlrun PRIVATE FTL_GUARD_ALLOC)
    target_include_directories(ftlrun PRIVATE include src)
    target_compile_options(ftlrun PRIVATE ${COMMON_FLAGS} -g)
    target_link_options(ftlrun PRIVATE -fuse-ld=lld -Wl,--exclude-libs,ALL -Wl,--gc-sections
        -Wl,--export-dynamic-symbol=free -Wl,--export-dynamic-symbol=realloc
        -Wl,--export-dynamic-symbol=malloc -Wl,--export-dynamic-symbol=calloc)
    target_link_libraries(ftlrun PRIVATE ${CXX_LIBS} EGL GLESv2 z pthread dl m)
else()
    add_library(ftlport SHARED ${CORE_SOURCES} src/platform/plat_android.c src/platform/android_audio.c)
    target_include_directories(ftlport PRIVATE include src)
    target_compile_options(ftlport PRIVATE ${COMMON_FLAGS} -O2 -g)
    target_link_options(ftlport PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,-z,max-page-size=16384
        -unwindlib=none -nostdlib++)
    target_link_libraries(ftlport PRIVATE ${CXX_LIBS} EGL GLESv3 android log aaudio z dl m)
endif()
