// PVRTC1 (2bpp / 4bpp) decoder -> RGBA8. iOS GPUs sample PVRTC natively;
// most Android GPUs can't, so compressed uploads are expanded on the CPU.
#include "pvrtc.h"

#include <stdlib.h>
#include <string.h>

typedef struct { int r, g, b, a; } px4;  // 5-bit colour, 4-bit alpha

static uint32_t twiddle(uint32_t xsize, uint32_t ysize, uint32_t x, uint32_t y) {
    uint32_t min_dim = xsize, max_val = y, out = 0, src = 1, dst = 1;
    int shift = 0;
    if (ysize < xsize) { min_dim = ysize; max_val = x; }
    while (src < min_dim) {
        if (y & src) out |= dst;
        if (x & src) out |= dst << 1;
        src <<= 1;
        dst <<= 2;
        shift++;
    }
    max_val >>= shift;
    return out | (max_val << (2 * shift));
}

static px4 color_a(uint32_t c) {
    px4 p;
    if (c & 0x8000) {
        p.r = (c & 0x7c00) >> 10;
        p.g = (c & 0x3e0) >> 5;
        p.b = (c & 0x1e) | ((c & 0x1e) >> 4);
        p.a = 0xf;
    } else {
        p.r = ((c & 0xf00) >> 7) | ((c & 0xf00) >> 11);
        p.g = ((c & 0xf0) >> 3) | ((c & 0xf0) >> 7);
        p.b = ((c & 0xe) << 1) | ((c & 0xe) >> 2);
        p.a = (c & 0x7000) >> 11;
    }
    return p;
}
static px4 color_b(uint32_t c) {
    px4 p;
    if (c & 0x80000000u) {
        p.r = (c & 0x7c000000) >> 26;
        p.g = (c & 0x3e00000) >> 21;
        p.b = (c & 0x1f0000) >> 16;
        p.a = 0xf;
    } else {
        p.r = ((c & 0xf000000) >> 23) | ((c & 0xf000000) >> 27);
        p.g = ((c & 0xf00000) >> 19) | ((c & 0xf00000) >> 23);
        p.b = ((c & 0xf0000) >> 15) | ((c & 0xf0000) >> 19);
        p.a = (c & 0x70000000) >> 27;
    }
    return p;
}

// Decode a full PVRTC1 image (w, h powers of two, at least one 2x2 block grid).
static void decode_full(const uint8_t *src, uint8_t *dst, int w, int h, int bpp2) {
    const int bw = bpp2 ? 8 : 4, bh = 4;
    const int nbx = w / bw, nby = h / bh;
    const uint32_t *words = (const uint32_t *)src;
    // Per-block colours and modulation (unpacked once).
    px4 *ca = malloc(sizeof(px4) * nbx * nby), *cb = malloc(sizeof(px4) * nbx * nby);
    uint8_t *mod = malloc((size_t)w * h);   // modulation weight 0..8, +16 = punch-through
    uint8_t *mmode = malloc((size_t)nbx * nby);
    for (int by = 0; by < nby; by++) {
        for (int bx = 0; bx < nbx; bx++) {
            uint32_t t = twiddle(nbx, nby, bx, by);
            uint32_t m = words[t * 2], c = words[t * 2 + 1];
            ca[by * nbx + bx] = color_a(c);
            cb[by * nbx + bx] = color_b(c);
            int mode = c & 1;
            mmode[by * nbx + bx] = (uint8_t)mode;
            if (!bpp2) {
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        int v = (m >> (2 * (y * 4 + x))) & 3;
                        int out;
                        if (mode) out = v == 0 ? 0 : v == 1 ? 4 : v == 2 ? (4 | 16) : 8;
                        else out = v == 0 ? 0 : v == 1 ? 3 : v == 2 ? 5 : 8;
                        mod[(by * 4 + y) * w + bx * 4 + x] = (uint8_t)out;
                    }
            } else if (!mode) {
                // 1 bit per texel
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++)
                        mod[(by * 4 + y) * w + bx * 8 + x] = ((m >> (y * 8 + x)) & 1) ? 8 : 0;
            } else {
                // 2 bits for checkerboard texels; others interpolated in a second pass.
                uint32_t bits = m;
                int sub = 1;  // 1 = H&V, 2 = H-only, 3 = V-only
                if (bits & 1) {
                    sub = (bits & (1u << 20)) ? 3 : 2;
                    if (bits & (1u << 21)) bits |= (1u << 20); else bits &= ~(1u << 20);
                }
                if (bits & 2) bits |= 1; else bits &= ~1u;
                mmode[by * nbx + bx] = (uint8_t)sub;
                static const int rep[4] = {0, 3, 5, 8};
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++)
                        if (((x ^ y) & 1) == 0) {
                            mod[(by * 4 + y) * w + bx * 8 + x] = (uint8_t)rep[bits & 3];
                            bits >>= 2;
                        }
            }
        }
    }
    if (bpp2) {
        // Fill interpolated texels of blocks in modes 1..3 (neighbours wrap).
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int bi = (y / 4) * nbx + x / 8;
                int sub = mmode[bi];
                if (!sub || ((x ^ y) & 1) == 0) continue;
                int xl = (x - 1 + w) % w, xr = (x + 1) % w, yu = (y - 1 + h) % h, yd = (y + 1) % h;
                int v;
                if (sub == 1) v = (mod[y * w + xl] + mod[y * w + xr] + mod[yu * w + x] + mod[yd * w + x] + 2) / 4;
                else if (sub == 2) v = (mod[y * w + xl] + mod[y * w + xr] + 1) / 2;
                else v = (mod[yu * w + x] + mod[yd * w + x] + 1) / 2;
                mod[y * w + x] = (uint8_t)v;
            }
        // Note: interpolated texels read stored neighbours only (stored and
        // interpolated texels alternate), so the in-place pass is order-safe.
    }
    // Colour reconstruction: bilinear between block centres (bw/2, bh/2 into each block).
    const int sx = bw, sy = bh;
    for (int y = 0; y < h; y++) {
        int fy = y - bh / 2;
        int by0 = fy >= 0 ? fy / bh : -1;
        int j = fy - by0 * bh;
        int byq = (by0 + nby) % nby, byr = (by0 + 1) % nby;
        for (int x = 0; x < w; x++) {
            int fx = x - bw / 2;
            int bx0 = fx >= 0 ? fx / bw : -1;
            int i = fx - bx0 * bw;
            int bxp = (bx0 + nbx) % nbx, bxq = (bx0 + 1) % nbx;
            const px4 *P = &ca[byq * nbx + bxp], *Q = &ca[byq * nbx + bxq], *R = &ca[byr * nbx + bxp], *S = &ca[byr * nbx + bxq];
            const px4 *P2 = &cb[byq * nbx + bxp], *Q2 = &cb[byq * nbx + bxq], *R2 = &cb[byr * nbx + bxp], *S2 = &cb[byr * nbx + bxq];
            int w00 = (sx - i) * (sy - j), w10 = i * (sy - j), w01 = (sx - i) * j, w11 = i * j;
            // sums are scaled by sx*sy (16 for 4bpp, 32 for 2bpp)
            int ar = P->r * w00 + Q->r * w10 + R->r * w01 + S->r * w11;
            int ag = P->g * w00 + Q->g * w10 + R->g * w01 + S->g * w11;
            int ab = P->b * w00 + Q->b * w10 + R->b * w01 + S->b * w11;
            int aa = P->a * w00 + Q->a * w10 + R->a * w01 + S->a * w11;
            int br = P2->r * w00 + Q2->r * w10 + R2->r * w01 + S2->r * w11;
            int bg = P2->g * w00 + Q2->g * w10 + R2->g * w01 + S2->g * w11;
            int bb = P2->b * w00 + Q2->b * w10 + R2->b * w01 + S2->b * w11;
            int ba = P2->a * w00 + Q2->a * w10 + R2->a * w01 + S2->a * w11;
            // expand to 8 bits (5->8: v*8 + v/4, 4->8: v*17) from the scaled sums
            int sh = bpp2 ? 5 : 4;  // log2(sx*sy)
            int A8r = (ar >> (sh + 2)) + (ar >> (sh - 3)), A8g = (ag >> (sh + 2)) + (ag >> (sh - 3)),
                A8b = (ab >> (sh + 2)) + (ab >> (sh - 3)), A8a = (aa >> sh) + (aa >> (sh - 4));
            int B8r = (br >> (sh + 2)) + (br >> (sh - 3)), B8g = (bg >> (sh + 2)) + (bg >> (sh - 3)),
                B8b = (bb >> (sh + 2)) + (bb >> (sh - 3)), B8a = (ba >> sh) + (ba >> (sh - 4));
            int mv = mod[y * w + x];
            int punch = mv & 16;
            mv &= 15;
            uint8_t *o = dst + ((size_t)y * w + x) * 4;
            o[0] = (uint8_t)((A8r * (8 - mv) + B8r * mv) / 8);
            o[1] = (uint8_t)((A8g * (8 - mv) + B8g * mv) / 8);
            o[2] = (uint8_t)((A8b * (8 - mv) + B8b * mv) / 8);
            o[3] = punch ? 0 : (uint8_t)((A8a * (8 - mv) + B8a * mv) / 8);
        }
    }
    free(ca);
    free(cb);
    free(mod);
    free(mmode);
}

int pvrtc_format_info(unsigned glformat, int *bpp2, int *has_alpha) {
    switch (glformat) {
    case 0x8C00: *bpp2 = 0; *has_alpha = 0; return 1;  // RGB 4bpp
    case 0x8C01: *bpp2 = 1; *has_alpha = 0; return 1;  // RGB 2bpp
    case 0x8C02: *bpp2 = 0; *has_alpha = 1; return 1;  // RGBA 4bpp
    case 0x8C03: *bpp2 = 1; *has_alpha = 1; return 1;  // RGBA 2bpp
    default: return 0;
    }
}

uint8_t *pvrtc_decode(const void *data, size_t size, int w, int h, unsigned glformat) {
    int bpp2, alpha;
    if (!pvrtc_format_info(glformat, &bpp2, &alpha)) return NULL;
    // PVRTC data is padded to at least 2x2 blocks (8x8 for 4bpp, 16x8 for 2bpp).
    int fw = w, fh = h;
    int minw = bpp2 ? 16 : 8, minh = 8;
    if (fw < minw) fw = minw;
    if (fh < minh) fh = minh;
    size_t need = (size_t)fw * fh * (bpp2 ? 2 : 4) / 8;
    if (size < need) return NULL;
    uint8_t *full = malloc((size_t)fw * fh * 4);
    decode_full(data, full, fw, fh, bpp2);
    if (!alpha)
        for (size_t i = 0; i < (size_t)fw * fh; i++) full[i * 4 + 3] = 255;
    if (fw == w && fh == h) return full;
    uint8_t *out = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) memcpy(out + (size_t)y * w * 4, full + (size_t)y * fw * 4, (size_t)w * 4);
    free(full);
    return out;
}
