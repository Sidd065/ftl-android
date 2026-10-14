// PNG images. The iPad build was compiled without its PNG reader (SIL's
// png_parse() is a stub that returns NULL): all its art ships as .tex
// textures. Mods add PC-style .png files, so put a real decoder behind it.
#include "png.h"

#include <string.h>

#include "../loader/macho.h"
#include "../patches.h"
#include "port_log.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "../third_party/stb_image.h"
#pragma clang diagnostic pop

// FTL iOS 1.6.21
#define G_PNG_PARSE 0x1000092d0ull  // void *png_parse(const void *data, uint32_t size, int mem_flags, int *w, int *h)
#define G_MEM_ALLOC 0x1001be780ull  // void *mem_alloc(size_t size, unsigned align, int flags): freed with mem_free

static mo_image *g_img;

// Returns straight-alpha RGBA pixels in a buffer from the game's allocator,
// which the caller frees with mem_free().
static void *png_parse(const void *data, uint32_t size, int mem_flags, int *w_ret, int *h_ret) {
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (!px) {
        LOGW("png: can't decode image (%s)", stbi_failure_reason());
        return NULL;
    }
    void *(*mem_alloc)(size_t, unsigned, unsigned long) =
        (void *(*)(size_t, unsigned, unsigned long))mo_ptr(g_img, G_MEM_ALLOC);
    size_t bytes = (size_t)w * h * 4;
    void *out = mem_alloc(bytes, 0, (unsigned long)mem_flags & ~1ul);
    if (out) {
        memcpy(out, px, bytes);
        *w_ret = w;
        *h_ret = h;
    }
    stbi_image_free(px);
    return out;
}

int png_install(mo_image *img) {
    const uint32_t *code = mo_ptr(img, G_PNG_PARSE);
    if (code[0] != 0xD2800000u || code[1] != 0xD65F03C0u) {  // mov x0, #0; ret
        LOGW("png: unexpected code at png_parse; PNG images from mods won't load");
        return -1;
    }
    g_img = img;
    return patch_branch(img, G_PNG_PARSE, (void *)png_parse);
}
