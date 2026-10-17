// Code patching infrastructure for the loaded game image.
#include "patches.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "loader/macho.h"
#include "port_log.h"

static uint8_t *g_tramp_page;
static size_t g_tramp_used;

static void *tramp_alloc(size_t n) {
    if (!g_tramp_page) {
        g_tramp_page = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_tramp_page == MAP_FAILED) { g_tramp_page = NULL; return NULL; }
    }
    if (g_tramp_used + n > 0x10000) return NULL;
    void *p = g_tramp_page + g_tramp_used;
    g_tramp_used += (n + 15) & ~(size_t)15;
    return p;
}

static int is_pc_relative(uint32_t ins) {
    if ((ins & 0x9f000000) == 0x90000000) return 1;  // adrp
    if ((ins & 0x9f000000) == 0x10000000) return 1;  // adr
    if ((ins & 0x7c000000) == 0x14000000) return 1;  // b / bl
    if ((ins & 0xff000010) == 0x54000000) return 1;  // b.cond
    if ((ins & 0x7e000000) == 0x34000000) return 1;  // cbz/cbnz
    if ((ins & 0x7e000000) == 0x36000000) return 1;  // tbz/tbnz
    if ((ins & 0x3b000000) == 0x18000000) return 1;  // ldr literal
    return 0;
}

// x17, not x16: Hyperspace hooks some of the same functions after us. It moves our first
// instruction (this load) elsewhere and goes back to the second with a jump through x16,
// so the branch here has to use a register that jump leaves alone.
static void write_abs_jump(uint32_t *p, void *target) {
    p[0] = 0x58000051;  // ldr x17, #8
    p[1] = 0xD61F0220;  // br x17
    memcpy(&p[2], &target, 8);
}

void *patch_hook(mo_image *img, uint64_t addr, void *hook, int want_original) {
    uint32_t *code = mo_ptr(img, addr);
    void *orig = NULL;
    if (want_original) {
        for (int i = 0; i < 4; i++)
            if (is_pc_relative(code[i])) {
                LOGE("patch_hook %#lx: instruction %d is pc-relative (%08x)", (unsigned long)addr, i, code[i]);
                return NULL;
            }
        uint32_t *t = tramp_alloc(32);
        if (!t) return NULL;
        memcpy(t, code, 16);
        write_abs_jump(t + 4, code + 4);
        __builtin___clear_cache((char *)t, (char *)(t + 8));
        orig = t;
    }
    write_abs_jump(code, hook);
    __builtin___clear_cache((char *)code, (char *)(code + 4));
    return want_original ? orig : (void *)1;
}

// Replace the instruction at `addr` with a branch to `target` through a near
// trampoline (works for tiny functions, e.g. a lone `ret`). Must run before mo_protect.
int patch_branch(mo_image *img, uint64_t addr, void *target) {
    if (!img->near_tramp || img->near_tramp_used + 16 > img->near_tramp_size) return -1;
    uint32_t *t = (uint32_t *)(img->near_tramp + img->near_tramp_used);
    img->near_tramp_used += 16;
    write_abs_jump(t, target);
    uint32_t *code = mo_ptr(img, addr);
    intptr_t delta = (intptr_t)((uint8_t *)t - (uint8_t *)code);
    if (delta >= (1 << 27) || delta < -(1 << 27)) return -1;
    *code = 0x14000000u | ((uint32_t)(delta >> 2) & 0x03ffffffu);
    __builtin___clear_cache((char *)code, (char *)(code + 1));
    return 0;
}

extern int ux_patches_apply(mo_image *img);
__attribute__((weak)) int ux_patches_apply(mo_image *img) { (void)img; return 0; }

extern void debug_hooks_apply(mo_image *img);
__attribute__((weak)) void debug_hooks_apply(mo_image *img) { (void)img; }

int patches_apply(mo_image *img) {
    debug_hooks_apply(img);
    return ux_patches_apply(img);
}
