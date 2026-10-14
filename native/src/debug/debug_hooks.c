// Debug-only hooks into game functions (FTL_DEBUGHOOKS=1) for tracing the
// resource loader. Addresses are for FTL iOS 1.6.21 (arm64).
#include <stdlib.h>
#include <string.h>

#include "../loader/macho.h"
#include "../patches.h"
#include "port_log.h"

// libc++ (Apple alternate layout) std::string -> C string
static const char *sstr(const void *s) {
    const unsigned char *b = s;
    if (!b) return "(null)";
    if (b[23] & 0x80) return *(const char *const *)s;
    return (const char *)s;
}

static int (*o_pkg_load_path)(void *, const char *, int, int);
static int h_pkg_load_path(void *pkg, const char *path, int a, int b) {
    int r = o_pkg_load_path(pkg, path, a, b);
    LOGI("dbg pkg_load_path(%s, %d, %#x) = %d", path, a, b, r);
    return r;
}
static int (*o_tex_load)(void *, void *);
static int h_tex_load(void *pkg, void *path) {
    int r = o_tex_load(pkg, path);
    LOGI("dbg tex_load(%s) = %d", sstr(path), r);
    return r;
}
static void (*o_tex_register)(void *, void *, int);
static void h_tex_register(void *self, void *name, int handle) {
    LOGI("dbg tex_register(%s, %d)", sstr(name), handle);
    o_tex_register(self, name, handle);
}
static void (*o_atlas_parse)(void *, void *, int, void *);
static void h_atlas_parse(void *self, void *data, int size, void *name) {
    LOGI("dbg atlas_parse(%s, %d bytes)", sstr(name), size);
    o_atlas_parse(self, data, size, name);
}

void debug_hooks_apply(mo_image *img) {
    if (!getenv("FTL_DEBUGHOOKS")) return;
    o_pkg_load_path = patch_hook(img, 0x10001c268, (void *)h_pkg_load_path, 1);
    o_tex_load = patch_hook(img, 0x100111304, (void *)h_tex_load, 1);
    o_tex_register = patch_hook(img, 0x100111558, (void *)h_tex_register, 1);
    o_atlas_parse = patch_hook(img, 0x10010dd80, (void *)h_atlas_parse, 1);
    LOGI("debug hooks installed");
}
