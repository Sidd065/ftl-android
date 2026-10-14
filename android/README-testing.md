# Testing in a redroid container (x86_64 host, no KVM)

Without hardware virtualisation the Android emulator won't run, but redroid
(Android 11 in Docker) does. With Google's ndk_translation copied in from the
API 30 x86_64 emulator image, it runs the arm64 APK through ARM-to-x86
translation.

    sudo modprobe binder_linux devices="binder,hwbinder,vndbinder"
    sudo docker run -itd --privileged --name redroid11 -v ~/ftl-build/redroid-data:/data -p 5555:5555 ftl-redroid11 \
      androidboot.redroid_width=2400 androidboot.redroid_height=1080 androidboot.redroid_dpi=420 \
      androidboot.redroid_gpu_mode=host ro.product.cpu.abilist=x86_64,arm64-v8a,x86,armeabi-v7a,armeabi \
      ro.product.cpu.abilist64=x86_64,arm64-v8a ro.dalvik.vm.isa.arm64=x86_64 ro.enable.native.bridge.exec=1 \
      ro.dalvik.vm.native.bridge=libndk_translation.so
    adb connect localhost:5555

`ftl-redroid11` = `redroid/redroid:11.0.0-latest` + /system/{lib64/libndk_translation*, lib64/arm64,
bin/arm64, bin/ndk_translation_program_runner_binfmt_misc*, etc/binfmt_misc, etc/init/ndk_translation.rc,
etc/ld.config.arm64.txt} copied from `system-images;android-30;google_apis;x86_64`.
