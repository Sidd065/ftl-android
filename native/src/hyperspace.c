// Hyperspace (the mod loader PC mods like Multiverse build on), when the player turns
// it on: libHyperspace.so from the APK's native library folder, started once the game
// is mapped, linked and protected, before any game code runs. It hooks the game in
// memory. Built from github.com/Sidd065/FTL-Hyperspace (branch android-arm64).
#include "hyperspace.h"

#include <dlfcn.h>
#include <stdio.h>

#include "hyperspace_host.h"
#include "loader/macho.h"
#include "port_log.h"

int __ftl_find_macho_unwind(uintptr_t pc, uintptr_t *mh, uintptr_t *textlen, uintptr_t *cu, size_t *culen,
                            uintptr_t *eh, size_t *ehlen);

static hs_android_wants_right_click_fn g_wants_right_click;
static hs_android_right_button_fn g_right_button;

static void hs_log(int level, const char *msg) {
    if (level >= 2) LOGE("%s", msg);
    else if (level == 1) LOGW("%s", msg);
    else LOGI("%s", msg);
}

int hyperspace_start(const char *lib, const char *log_dir, const char *data_dir, mo_image *img, char *err,
                     size_t errsize) {
    LOGI("hyperspace: loading %s", lib);
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        snprintf(err, errsize, "Hyperspace couldn't be loaded: %s", dlerror());
        return -1;
    }
    hs_android_init_fn init = (hs_android_init_fn)dlsym(h, "hs_android_init");
    if (!init) {
        snprintf(err, errsize, "This libHyperspace.so isn't built for the Android port.");
        return -1;
    }
    hs_host_info info = {
        .version = HS_HOST_VERSION,
        .size = sizeof info,
        .mach_header = img->base,
        .slide = img->slide,
        .image_size = img->map_size,
        .log_dir = log_dir,
        .data_dir = data_dir,
        .log = hs_log,
        .find_macho_unwind = __ftl_find_macho_unwind,
    };
    char why[1024] = "";
    if (init(&info, why, sizeof why) != 0) {
        snprintf(err, errsize, "Hyperspace couldn't start: %s", why[0] ? why : "unknown error (see zhl.log)");
        return -1;
    }
    g_wants_right_click = (hs_android_wants_right_click_fn)dlsym(h, "hs_android_wants_right_click");
    g_right_button = (hs_android_right_button_fn)dlsym(h, "hs_android_right_button");
    LOGI("hyperspace: started");
    return 0;
}

bool hyperspace_wants_right_click(void) {
    return g_wants_right_click && g_right_button && g_wants_right_click();
}

void hyperspace_right_button(int action, double x, double y) {
    if (g_right_button) g_right_button(action, (int)x, (int)y);
}
