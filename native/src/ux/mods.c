// Mods. The app patches its copy of ftl.dat with ftlman, the same way mods are
// installed on PC, so modded data files simply replace the originals. Images
// need help: mods add PC-style .png files, while the iPad build looks for its
// own .tex textures first and keeps small images in atlases. So when mods are
// applied, the game's ftl.dat reader is taught to
//  - hide x.tex when a mod added x.png: asked for an image, the game tries
//    the .tex name first, then the .png one (the loader goes by the name, so
//    the .png data can't simply be served under the .tex name),
//  - leave x.tex out of the file list for the same reason,
//  - read the atlas lists from a small overlay the app writes (Mods.java):
//    the originals minus the images that mods replaced.
// The overlay is a data file of zlib-compressed files and an index of
// "offset, size, length, added, path" lines.
#include "mods.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "../loader/macho.h"
#include "../patches.h"
#include "port_log.h"

// SIL's package module for ftl.dat (FTL iOS 1.6.21)
#define G_PKG_LIST_START 0x1000f4ec8ull  // void list_files_start(PackageModuleInfo *)
#define G_PKG_LIST_NEXT 0x1000f4edcull   // const char *list_files_next(PackageModuleInfo *)
#define G_PKG_FILE_INFO 0x1000f4f28ull   // int file_info(module, path, &fh, &pos, &len, &comp, &size)
#define G_SYS_FILE_OPEN 0x10001b19cull   // SysFile *sys_file_open(const char *path)
// PKG module state (module + 0x50): +0x20 index (20-byte entries, path offset
// in the low 24 bits of the second word), +0x28 entry count, +0x2c list
// position, +0x30 path table.

typedef struct {
    char *path;
    uint64_t off;
    uint32_t size, len;  // stored (compressed) size, real length
    int added;           // not in ftl.dat: list it after the game's own files
} ov_entry;

static mo_image *g_img;
static ov_entry *g_ov;
static int g_nov;
static char g_data_path[600];
static void *g_data_file;  // SysFile *, opened on first use
static pthread_once_t g_open_once = PTHREAD_ONCE_INIT;
static int g_list_pos;

typedef int (*file_info_fn)(void *module, const char *path, void **fh, int64_t *pos, uint32_t *len, uint32_t *comp,
                            uint32_t *size);
static file_info_fn g_file_info;

static int by_path(const void *a, const void *b) {
    return strcasecmp(((const ov_entry *)a)->path, ((const ov_entry *)b)->path);
}

static void open_data(void) {
    void *(*sys_file_open)(const char *) = (void *(*)(const char *))mo_ptr(g_img, G_SYS_FILE_OPEN);
    g_data_file = sys_file_open(g_data_path);
    if (!g_data_file) LOGE("mods: can't open %s", g_data_path);
}

// "x.tex" -> "x.png" in buf; false if path isn't a .tex name.
static bool png_name(const char *path, char *buf, size_t n) {
    size_t len = strlen(path);
    if (len < 5 || len >= n || strcasecmp(path + len - 4, ".tex")) return false;
    memcpy(buf, path, len - 4);
    memcpy(buf + len - 4, ".png", 5);
    return true;
}

static bool pkg_has(void *module, const char *path) {
    void *fh;
    int64_t pos;
    uint32_t len, comp, size;
    return g_file_info(module, path, &fh, &pos, &len, &comp, &size) != 0;
}

static int file_info_hook(void *module, const char *path, void **fh, int64_t *pos, uint32_t *len, uint32_t *comp,
                          uint32_t *size) {
    if (path) {
        ov_entry key = {.path = (char *)path};
        const ov_entry *e = bsearch(&key, g_ov, g_nov, sizeof *g_ov, by_path);
        if (e) {
            pthread_once(&g_open_once, open_data);
            if (g_data_file) {
                *fh = g_data_file;
                *pos = (int64_t)e->off;
                *len = e->size;
                *comp = 1;
                *size = e->len;
                return 1;
            }
        }
        // A mod's image wins over the iPad texture of the same name.
        char png[1024];
        if (png_name(path, png, sizeof png) && pkg_has(module, png)) return 0;
    }
    return g_file_info(module, path, fh, pos, len, comp, size);
}

static void list_start_hook(uint8_t *module) {
    uint8_t *st = module ? *(uint8_t **)(module + 0x50) : NULL;
    if (st) *(int32_t *)(st + 0x2c) = 0;
    g_list_pos = 0;
}

static const char *list_next_hook(uint8_t *module) {
    uint8_t *st = module ? *(uint8_t **)(module + 0x50) : NULL;
    if (!st) return NULL;
    for (;;) {
        int32_t i = *(int32_t *)(st + 0x2c);
        if (i >= *(int32_t *)(st + 0x28)) break;
        *(int32_t *)(st + 0x2c) = i + 1;
        uint32_t po = *(uint32_t *)(*(uint8_t **)(st + 0x20) + (size_t)i * 20 + 4);
        const char *name = (const char *)(*(uint8_t **)(st + 0x30) + (po & 0xffffff));
        char png[1024];
        if (png_name(name, png, sizeof png) && pkg_has(module, png)) continue;  // listed as the .png
        return name;
    }
    while (g_list_pos < g_nov) {
        const ov_entry *e = &g_ov[g_list_pos++];
        if (e->added) return e->path;
    }
    return NULL;
}

static int load_index(const char *idx_path) {
    FILE *f = fopen(idx_path, "r");
    if (!f) {
        LOGE("mods: can't read %s", idx_path);
        return -1;
    }
    struct stat st;
    if (stat(g_data_path, &st) != 0) {
        LOGE("mods: %s is missing", g_data_path);
        fclose(f);
        return -1;
    }
    char line[1024];
    int cap = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strcspn(line, "\r\n");
        line[n] = 0;
        unsigned long long off, size, len;
        int added, at = 0;
        if (sscanf(line, "%llu\t%llu\t%llu\t%d\t%n", &off, &size, &len, &added, &at) != 4 || !at || !line[at] ||
            off + size > (unsigned long long)st.st_size || len > 0x7fffffff) {
            bad++;
            continue;
        }
        if (g_nov == cap) {
            cap = cap ? cap * 2 : 256;
            g_ov = realloc(g_ov, (size_t)cap * sizeof *g_ov);
        }
        g_ov[g_nov++] = (ov_entry){strdup(line + at), off, (uint32_t)size, (uint32_t)len, added};
    }
    fclose(f);
    qsort(g_ov, g_nov, sizeof *g_ov, by_path);
    if (bad) LOGW("mods: skipped %d bad index lines", bad);
    return 0;
}

int mods_install(mo_image *img, const char *index_path) {
    if (!index_path || !*index_path) return 0;
    size_t n = strlen(index_path);
    if (n < 4 || n >= sizeof g_data_path) return -1;
    // overlay.idx -> overlay.dat
    memcpy(g_data_path, index_path, n - 3);
    strcpy(g_data_path + n - 3, "dat");
    g_img = img;
    if (load_index(index_path) != 0) return -1;
    // Expected code, so a different build isn't patched blindly.
    const uint32_t *fi = mo_ptr(img, G_PKG_FILE_INFO), *ls = mo_ptr(img, G_PKG_LIST_START),
                   *ln = mo_ptr(img, G_PKG_LIST_NEXT);
    if (fi[0] != 0xD10243FFu || ls[0] != 0xB4000080u || ln[0] != 0xB4000200u) {
        LOGW("mods: unexpected code in the ftl.dat reader; mod images may not show");
        return -1;
    }
    g_file_info = (file_info_fn)patch_hook(img, G_PKG_FILE_INFO, (void *)file_info_hook, 1);
    if (!g_file_info) return -1;
    patch_hook(img, G_PKG_LIST_START, (void *)list_start_hook, 0);
    patch_hook(img, G_PKG_LIST_NEXT, (void *)list_next_hook, 0);
    LOGI("mods: on (%d adjusted atlas files)", g_nov);
    return 0;
}
