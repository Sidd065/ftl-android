// Phone UX layer for the FTL iPad build:
//  - screen layout / render resolution (render at a multiple of 768 px tall)
//  - zoomable view (pinch to zoom, two-finger pan) presented by present.c
//  - gestures: two-finger tap toggles pause; single touches go to the game
//  - Android Back -> Escape; hardware keyboard + mouse -> the game's own
//    (desktop-heritage) key and mouse events
//  - game logger hook for diagnostics
#include "ux.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../fw/fw.h"
#include "../fw/present.h"
#include "../loader/macho.h"
#include "../patches.h"
#include "../platform/platform.h"
#include "../port_main.h"
#include "overlay.h"
#include "ux_present.h"
#include "port_log.h"

// ---- game addresses (FTL iOS 1.6.21, arm64) ---------------------------------------
#define G_LOG_FN 0x10006051cull       // printf-style logger, compiled to `ret`
#define G_POST_EVENT 0x100117268ull   // void post_event(sil_event *)
#define G_CAPP_PTR 0x100241fc0ull     // CApp *
#define G_TIME_BASE 0x100250c88ull    // double, event timestamp base (s)
#define G_TOGGLE_PAUSE 0x100180240ull // void (CommandGui *): the pause hotkey's action
#define G_HOTKEY 0x100040728ull       // int (const std::string *action): `return 0` on iOS

// CApp fields
#define CAPP_GUI 0x10       // CommandGui *
#define CAPP_MENU 0x20      // MainMenu (first byte: open)
#define CAPP_STARTUP 0x40b8 // language chooser / startup screen up (game ignores keys)
// MainMenu fields: sub-screens that Escape closes
static const int MENU_SUBSCREENS[] = {0x26b8, 0x3d00, 0x3fd0, 0x26c8, 0x15f0};
#define GUI_PAUSED 0x3c58   // CommandGui: bool paused

typedef struct {
    int32_t cls, type;
    double time;
    int32_t i0;  // key code / touch id / (float bits) mouse x
    float f1, f2, f3, f4;
    int32_t i5;
} sil_event;
_Static_assert(sizeof(sil_event) == 40, "sil_event");

enum { SIL_CLS_KEY = 2 };
enum { SIL_KEY_DOWN = 7, SIL_KEY_UP = 8 };
#define SIL_KEY_SPACE 39
#define SIL_KEY_ESCAPE 104

void darwin_exit(int code);

static mo_image *g_img;
static float g_dpi = 400.0f;
static bool g_mag_auto;              // magnifier wanted in "automatic" mode (small screen)
static bool g_two_finger_pause = true;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---------------------------------------------------------------------------------
// game logger -> our log
int darwin_vsnprintf_stack(char *buf, size_t n, const char *fmt, void *stack);
void game_log_impl(const char *fmt, void *stack) {
    char buf[1024];
    if (!fmt) return;
    darwin_vsnprintf_stack(buf, sizeof buf, fmt, stack);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    LOGI("[FTL] %s", buf);
}
extern void game_log_tramp(void);
__asm__(".text\n.globl game_log_tramp\n.p2align 2\ngame_log_tramp:\n  mov x1, sp\n  b game_log_impl\n");

// ---------------------------------------------------------------------------------
// keyboard shortcuts: the iOS build keeps the desktop hotkey handling but its
// binding lookup always answers 0. Answer with desktop-style defaults (SDL 1.2
// keysyms) once a hardware key has been used, so touch-only players don't see
// key hints in tooltips.
static bool g_keyboard_used;
enum { K_SPACE = 32, K_F1 = 282 };
static const struct { const char *action; int key; } HOTKEYS[] = {
    {"pause", K_SPACE}, {"jump", 'j'}, {"ship_info", 'u'}, {"ship_crew", 'c'}, {"ship_inv", 'i'},
    {"autofire", 'v'}, {"open", 'z'}, {"close", 'x'}, {"send_tele", 't'}, {"ret_tele", 'r'},
    {"lockdown", 'l'}, {"savePositions", '['}, {"loadPositions", ']'}, {"crew_all", '`'},
    // systems (Shift + key removes power)
    {"shields", 'a'}, {"engines", 's'}, {"oxygen", 'f'}, {"medbay", 'd'}, {"clonebay", 'd'},
    {"weapons", 'w'}, {"drones", 'e'}, {"teleporter", 'g'}, {"cloaking", 'h'}, {"artillery", 'y'},
    {"battery", 'b'}, {"mind", 'n'}, {"hacking", 'm'},
    // weapon slots then drone slots (Shift + key removes power)
    {"power_1", '1'}, {"power_2", '2'}, {"power_3", '3'}, {"power_4", '4'},
    {"power_5", '5'}, {"power_6", '6'}, {"power_7", '7'}, {"power_8", '8'},
    {"crew1", K_F1}, {"crew2", K_F1 + 1}, {"crew3", K_F1 + 2}, {"crew4", K_F1 + 3},
    {"crew5", K_F1 + 4}, {"crew6", K_F1 + 5}, {"crew7", K_F1 + 6}, {"crew8", K_F1 + 7},
};
// libc++ std::string (Apple's alternate layout)
static const char *cxx_str(const void *s, size_t *len) {
    const uint8_t *b = s;
    if (b[23] & 0x80) {
        *len = *(const size_t *)(b + 8);
        return *(const char *const *)b;
    }
    *len = b[23];
    return (const char *)b;
}
static int hotkey_lookup(const void *action) {
    if (!g_keyboard_used || !action) return 0;
    size_t n;
    const char *a = cxx_str(action, &n);
    for (size_t i = 0; i < sizeof HOTKEYS / sizeof *HOTKEYS; i++)
        if (strlen(HOTKEYS[i].action) == n && !memcmp(HOTKEYS[i].action, a, n)) return HOTKEYS[i].key;
    static char seen[48][32];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (!strncmp(seen[i], a, n) && !seen[i][n]) return 0;
    if (nseen < 48 && n < 32) {
        memcpy(seen[nseen], a, n);
        seen[nseen++][n] = 0;
        LOGI("ux: no default key for action '%.*s'", (int)n, a);
    }
    return 0;
}

// The addresses above are for one build: FTL for iPad 1.6.21 (arm64 slice
// LC_UUID below). Any other build runs without the game-specific extras.
static const uint8_t KNOWN_UUID[16] = {0x87, 0x3d, 0x2b, 0x34, 0xd7, 0x07, 0x3d, 0x6b,
                                       0xab, 0x83, 0x26, 0x7b, 0xf4, 0xc5, 0xbb, 0xc0};
static bool image_is_known_build(mo_image *img) {
    const uint8_t *h = mo_ptr(img, 0x100000000ull);
    if (!h || *(const uint32_t *)h != 0xfeedfacfu) return false;
    uint32_t ncmds = *(const uint32_t *)(h + 16);
    const uint8_t *lc = h + 32;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = *(const uint32_t *)lc, size = *(const uint32_t *)(lc + 4);
        if (cmd == 0x1b) return !memcmp(lc + 8, KNOWN_UUID, 16);  // LC_UUID
        lc += size;
    }
    return false;
}

int ux_patches_apply(mo_image *img) {
    if (!image_is_known_build(img)) {
        LOGW("ux: not the FTL 1.6.21 iPad build; pause gesture, Back/quit, keyboard and logger hooks disabled");
        return 0;
    }
    g_img = img;
    int fails = 0;
    uint32_t *log_fn = mo_ptr(img, G_LOG_FN);
    if (*log_fn == 0xD65F03C0u)  // ret: expected build
        fails += patch_branch(img, G_LOG_FN, (void *)game_log_tramp) != 0;
    else
        LOGW("ux: unexpected code at game logger; not hooking");
    uint32_t *hk = mo_ptr(img, G_HOTKEY);
    if (hk[0] == 0x52800000u && hk[1] == 0xD65F03C0u)  // mov w0, #0; ret
        fails += patch_branch(img, G_HOTKEY, (void *)hotkey_lookup) != 0;
    else
        LOGW("ux: unexpected code at hotkey lookup; keyboard shortcuts disabled");
    return fails ? -1 : 0;
}

// ---------------------------------------------------------------------------------
// game state probes
static void *capp(void) { return g_img ? *(void **)mo_ptr(g_img, G_CAPP_PTR) : NULL; }
bool ux_in_main_menu(void) {
    uint8_t *a = capp();
    return a && (a[CAPP_MENU] || a[CAPP_STARTUP]);
}
bool ux_in_game(void) {
    uint8_t *a = capp();
    return a && !a[CAPP_MENU] && !a[CAPP_STARTUP];
}
// main menu showing with no sub-screen (options, stats, hangar, ...) open
static bool at_top_menu(void) {
    uint8_t *a = capp();
    if (!a || !a[CAPP_MENU] || a[CAPP_STARTUP]) return false;
    for (size_t i = 0; i < sizeof MENU_SUBSCREENS / sizeof *MENU_SUBSCREENS; i++)
        if (a[CAPP_MENU + MENU_SUBSCREENS[i]]) return false;
    return true;
}
// Toggle pause like the desktop hotkey. Returns the new state (-1: not in game).
static int toggle_pause(void) {
    uint8_t *a = capp();
    if (!a || a[CAPP_MENU] || a[CAPP_STARTUP]) return -1;
    uint8_t *gui = *(uint8_t **)(a + CAPP_GUI);
    if (!gui) return -1;
    ((void (*)(void *))mo_ptr(g_img, G_TOGGLE_PAUSE))(gui);
    return gui[GUI_PAUSED] != 0;
}

// ---------------------------------------------------------------------------------
// event injection (main thread)
static void post_event(sil_event *e) {
    if (!g_img) return;
    double base = *(double *)mo_ptr(g_img, G_TIME_BASE);
    e->time = base > 0 ? now_s() - base : 0;
    void (*fn)(sil_event *) = (void (*)(sil_event *))mo_ptr(g_img, G_POST_EVENT);
    fn(e);
}
void ux_key(int sil_code, bool down) {
    if (sil_code <= 0) return;
    sil_event e = {SIL_CLS_KEY, down ? SIL_KEY_DOWN : SIL_KEY_UP, 0, sil_code, 0, 0, 0, 0, 0};
    post_event(&e);
}
static void key_tap(int sil_code) {
    ux_key(sil_code, true);
    ux_key(sil_code, false);
}
static void cursor_hide(void);
static void zoom_at(double factor, double sx, double sy);

// ---------------------------------------------------------------------------------
// view transform: the part of the game image shown on screen (game pixels)
typedef struct { double x0, y0, x1, y1; } rectd;
static pthread_mutex_t g_view_lock = PTHREAD_MUTEX_INITIALIZER;
static rectd g_view_cur, g_view_from, g_view_to;
static double g_anim_t0, g_anim_dur;
static const double ZOOM_MAX = 3.0;

static rectd full_view(void) { return (rectd){0, 0, g_display.render_w, g_display.render_h}; }
static double view_zoom(rectd v) { return g_display.render_w / (v.x1 - v.x0); }

static rectd clamp_view(rectd v) {
    double w = v.x1 - v.x0, h = v.y1 - v.y0, rw = g_display.render_w, rh = g_display.render_h;
    if (w > rw) { w = rw; h = rh; }
    if (v.x0 < 0) v.x0 = 0;
    if (v.y0 < 0) v.y0 = 0;
    if (v.x0 + w > rw) v.x0 = rw - w;
    if (v.y0 + h > rh) v.y0 = rh - h;
    v.x1 = v.x0 + w;
    v.y1 = v.y0 + h;
    return v;
}
static void set_view(rectd v, double anim) {
    pthread_mutex_lock(&g_view_lock);
    g_view_from = g_view_cur;
    g_view_to = clamp_view(v);
    g_anim_t0 = now_s();
    g_anim_dur = anim;
    if (anim <= 0) g_view_cur = g_view_to;
    pthread_mutex_unlock(&g_view_lock);
}
// render thread: current (animated) view
static rectd current_view(void) {
    pthread_mutex_lock(&g_view_lock);
    if (g_anim_dur > 0) {
        double t = (now_s() - g_anim_t0) / g_anim_dur;
        if (t >= 1) { g_view_cur = g_view_to; g_anim_dur = 0; }
        else {
            double s = t * t * (3 - 2 * t);
            g_view_cur.x0 = g_view_from.x0 + (g_view_to.x0 - g_view_from.x0) * s;
            g_view_cur.y0 = g_view_from.y0 + (g_view_to.y0 - g_view_from.y0) * s;
            g_view_cur.x1 = g_view_from.x1 + (g_view_to.x1 - g_view_from.x1) * s;
            g_view_cur.y1 = g_view_from.y1 + (g_view_to.y1 - g_view_from.y1) * s;
        }
    }
    rectd v = g_view_cur;
    pthread_mutex_unlock(&g_view_lock);
    return v;
}
static rectd target_view(void) {
    pthread_mutex_lock(&g_view_lock);
    rectd v = g_view_to;
    pthread_mutex_unlock(&g_view_lock);
    return v;
}
void ux_reset_zoom(bool animate) { set_view(full_view(), animate ? 0.18 : 0); }

// surface px -> game px under the (target) view
static void surface_to_game(double sx, double sy, double *gx, double *gy) {
    present_rect d = present_get_dest();
    rectd v = target_view();
    double u = d.w > 0 ? (sx - d.x) / d.w : 0, w = d.h > 0 ? (sy - d.y) / d.h : 0;
    *gx = v.x0 + u * (v.x1 - v.x0);
    *gy = v.y0 + w * (v.y1 - v.y0);
}

// ---------------------------------------------------------------------------------
// layout
void ux_configure(int sw, int sh, int refresh_hz, const float *layout) {
    float l = layout ? layout[0] : 0, t = layout ? layout[1] : 0, r = layout ? layout[2] : 0, b = layout ? layout[3] : 0;
    if (layout && layout[4] > 0) g_dpi = layout[4];
    int area_w = sw - (int)(l + r), area_h = sh - (int)(t + b);
    if (area_w <= 0 || area_h <= 0) {
        area_w = sw;
        area_h = sh;
        l = t = 0;
    }
    // The game lays out a canvas 768 units tall (width follows the aspect
    // ratio). Render at 2x for typical phones/tablets so text stays crisp when
    // downscaled and zoomed; 1x for small surfaces.
    int rh = area_h >= 900 ? 1536 : 768;
    if (layout && layout[5] > 0) rh = (int)layout[5];
    int rw = ((int)lround((double)rh * area_w / area_h)) & ~1;
    g_display.render_w = rw;
    g_display.render_h = rh;
    g_display.scale = 2.0;
    // FTL is built for 60 Hz; faster panels would only cost battery.
    fw_configure_display(sw, sh, refresh_hz > 0 && refresh_hz < 60 ? refresh_hz : 60);
    present_set_dest((present_rect){(int)l, (int)t, area_w, area_h});
    g_view_cur = g_view_to = full_view();
    present_set_view_fn(ux_present_view);
    present_set_overlay(overlay_draw);
    // Magnifier: on by default where the game is small (phones), layout[6]: 0 auto, 1 on, 2 off.
    int mag = layout ? (int)layout[6] : 0;
    double inches = area_h / g_dpi;
    g_mag_auto = inches < 3.6;
    overlay_loupe_enable(mag == 1 || (mag == 0 && g_mag_auto));
    LOGI("ux: surface %dx%d, game area %dx%d at (%d,%d), render %dx%d, %.0f dpi (%.1f in tall), magnifier %s", sw, sh,
         area_w, area_h, (int)l, (int)t, rw, rh, g_dpi, inches, mag == 1 || (mag == 0 && inches < 3.6) ? "on" : "off");
}

// ---------------------------------------------------------------------------------
// touch routing + gestures (main thread)
#define MAXP 10
typedef struct {
    bool active;
    long id;
    double sx, sy;     // current surface position
    double x0, y0;     // start position
    double t0;
    bool to_game;      // forwarded to the game (and not cancelled)
} ptr_t;
static ptr_t P[MAXP];
static enum { G_IDLE, G_SINGLE, G_GESTURE } g_mode;
static int g_ga = -1, g_gb = -1;          // pointer slots used by the gesture
static double g_d0, g_z0, g_ax, g_ay;    // initial finger distance, zoom, anchor (game px)
static double g_gt0, g_gmove;            // gesture start time, max finger travel (px)
static bool g_minimap_press;

static int slot_of(long id) {
    for (int i = 0; i < MAXP; i++)
        if (P[i].active && P[i].id == id) return i;
    return -1;
}
static int free_slot(void) {
    for (int i = 0; i < MAXP; i++)
        if (!P[i].active) return i;
    return -1;
}
static int active_count(void) {
    int n = 0;
    for (int i = 0; i < MAXP; i++) n += P[i].active;
    return n;
}
static void forward(int phase, ptr_t *p, double t) {
    double gx, gy;
    surface_to_game(p->sx, p->sy, &gx, &gy);
    fw_dispatch_touch(phase, p->id, gx, gy, t);
}
static double dist(ptr_t *a, ptr_t *b) { return hypot(a->sx - b->sx, a->sy - b->sy); }

static void gesture_begin(int a, int b) {
    g_mode = G_GESTURE;
    g_ga = a;
    g_gb = b;
    g_d0 = dist(&P[a], &P[b]);
    if (g_d0 < 1) g_d0 = 1;
    rectd v = target_view();
    g_z0 = view_zoom(v);
    double mx = (P[a].sx + P[b].sx) / 2, my = (P[a].sy + P[b].sy) / 2;
    surface_to_game(mx, my, &g_ax, &g_ay);
    g_gt0 = now_s();
    g_gmove = 0;
}

static void gesture_update(void) {
    ptr_t *a = &P[g_ga], *b = &P[g_gb];
    double z = g_z0 * dist(a, b) / g_d0;
    if (z < 1) z = 1;
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    present_rect d = present_get_dest();
    double mx = (a->sx + b->sx) / 2, my = (a->sy + b->sy) / 2;
    double vw = g_display.render_w / z, vh = g_display.render_h / z;
    // keep the anchor (game point under the fingers' midpoint) under the midpoint
    double x0 = g_ax - (mx - d.x) / d.w * vw, y0 = g_ay - (my - d.y) / d.h * vh;
    set_view((rectd){x0, y0, x0 + vw, y0 + vh}, 0);
    double m = fmax(hypot(a->sx - a->x0, a->sy - a->y0), hypot(b->sx - b->x0, b->sy - b->y0));
    if (m > g_gmove) g_gmove = m;
}

static void gesture_end(void) {
    double dur = now_s() - g_gt0;
    double slop = g_dpi / 6.0;  // ~4 mm
    if (dur < 0.35 && g_gmove < slop) {
        // two-finger tap: pause / unpause
        int paused = g_two_finger_pause ? toggle_pause() : -1;
        LOGI("ux: two-finger tap (%.0f ms) -> paused %d", dur * 1000, paused);
        if (paused >= 0) overlay_flash(paused ? OVERLAY_ICON_PAUSE : OVERLAY_ICON_PLAY);
    } else {
        rectd v = target_view();
        LOGI("ux: pinch/pan end, zoom %.2f (%.0f ms, moved %.0f px)", view_zoom(v), dur * 1000, g_gmove);
        if (view_zoom(v) < 1.08) ux_reset_zoom(true);
    }
    g_ga = g_gb = -1;
}

void ux_handle_touch(int phase, long id, float sx, float sy, double t) {
    if (phase == TOUCH_BEGAN) {
        cursor_hide();
        int s = free_slot();
        if (s < 0) return;
        P[s] = (ptr_t){true, id, sx, sy, sx, sy, t, false};
        int n = active_count();
        if (n == 1) {
            // Tap on the zoom minimap resets the zoom.
            if (overlay_minimap_hit(sx, sy)) {
                g_minimap_press = true;
                g_mode = G_SINGLE;
                return;
            }
            g_mode = G_SINGLE;
            P[s].to_game = true;
            forward(TOUCH_BEGAN, &P[s], t);
            overlay_loupe_touch(1, sx, sy);
        } else if (g_mode != G_GESTURE) {
            // second finger: take over as a pinch/pan gesture
            int other = -1;
            for (int i = 0; i < MAXP; i++)
                if (P[i].active && i != s) other = i;
            if (other >= 0 && P[other].to_game) {
                forward(TOUCH_CANCELLED, &P[other], t);
                P[other].to_game = false;
                overlay_loupe_touch(0, 0, 0);
            }
            g_minimap_press = false;
            if (other >= 0) gesture_begin(other, s);
        }
        return;
    }
    int s = slot_of(id);
    if (s < 0) return;
    P[s].sx = sx;
    P[s].sy = sy;
    if (phase == TOUCH_MOVED) {
        if (g_mode == G_GESTURE && (s == g_ga || s == g_gb)) gesture_update();
        else if (P[s].to_game) {
            forward(TOUCH_MOVED, &P[s], t);
            overlay_loupe_touch(2, sx, sy);
        }
        return;
    }
    // ended / cancelled
    if (P[s].to_game) {
        forward(phase, &P[s], t);
        overlay_loupe_touch(0, sx, sy);
    }
    if (g_minimap_press && phase == TOUCH_ENDED && overlay_minimap_hit(sx, sy)) ux_reset_zoom(true);
    g_minimap_press = false;
    bool was_gesture_finger = g_mode == G_GESTURE && (s == g_ga || s == g_gb);
    P[s].active = false;
    if (was_gesture_finger) gesture_end();
    if (active_count() == 0) g_mode = G_IDLE;
}

// ---------------------------------------------------------------------------------
// keys & mouse from the platform
void ux_handle_back(void) {
    uint8_t *a = capp();
    if (a && a[CAPP_MENU]) {
        uint8_t *m = a + CAPP_MENU;
        LOGI("ux: back in menu (sub-screens %d %d %d %d %d)", m[0x26b8], m[0x3d00], m[0x3fd0], m[0x26c8], m[0x15f0]);
    }
    // Nothing left to close on the title screen: offer to quit.
    if (at_top_menu()) {
        plat_request_quit();
        return;
    }
    // Escape closes windows / opens the in-game menu, like on desktop.
    key_tap(SIL_KEY_ESCAPE);
}

// Quit like iOS would terminate the app: the game saves, then calls exit().
static void quit_fallback(void *u) {
    (void)u;
    LOGW("ux: game kept running after terminate; exiting");
    darwin_exit(0);
}
void ux_quit(void) {
    fw_app_will_resign_active();
    fw_app_will_terminate();
    fw_run_on_main(quit_fallback, NULL, false);
}

// Android KEYCODE_* -> SIL key codes (the game's own SIL -> SDL table is at 0x1000090a8)
int ux_android_key_to_sil(int k) {
    if (k >= 29 && k <= 54) return 1 + (k - 29);  // A..Z
    if (k >= 7 && k <= 16) return 27 + (k - 7);   // 0..9
    switch (k) {
    case 61: return 37;   // TAB
    case 66: return 38;   // ENTER
    case 62: return 39;   // SPACE
    case 75: return 46;   // '
    case 55: return 51;   // ,
    case 69: return 52;   // -
    case 56: return 53;   // .
    case 76: return 54;   // /
    case 74: return 56;   // ;
    case 70: return 58;   // =
    case 71: return 62;   // [
    case 73: return 63;   // backslash
    case 72: return 64;   // ]
    case 68: return 67;   // `
    case 59: return 75;   // LSHIFT
    case 60: return 76;   // RSHIFT
    case 113: return 77;  // LCTRL
    case 114: return 78;  // RCTRL
    case 57: return 79;   // LALT
    case 58: return 80;   // RALT
    case 117: return 81;  // META_LEFT
    case 118: return 82;  // META_RIGHT
    case 143: return 85;  // NUM_LOCK
    case 115: return 86;  // CAPS_LOCK
    case 116: return 87;  // SCROLL_LOCK
    case 67: return 92;   // DEL (backspace)
    case 124: return 94;  // INSERT
    case 112: return 95;  // FORWARD_DEL
    case 122: return 96;  // MOVE_HOME
    case 123: return 97;  // MOVE_END
    case 92: return 98;   // PAGE_UP
    case 93: return 99;   // PAGE_DOWN
    case 19: return 100;  // DPAD_UP
    case 20: return 101;  // DPAD_DOWN
    case 21: return 102;  // DPAD_LEFT
    case 22: return 103;  // DPAD_RIGHT
    case 111: return 104; // ESCAPE
    case 121: return 126; // BREAK/PAUSE
    case 82: return 127;  // MENU
    case 160: return 144; // NUMPAD_ENTER
    default: break;
    }
    if (k >= 131 && k <= 142) return 105 + (k - 131);  // F1..F12
    if (k >= 144 && k <= 153) return 128 + (k - 144);  // NUMPAD_0..9
    return 0;
}

void ux_handle_key(int android_keycode, bool down) {
    int sil = ux_android_key_to_sil(android_keycode);
    if (!sil) return;
    if (!g_keyboard_used) LOGI("ux: hardware keyboard in use; enabling shortcuts");
    g_keyboard_used = true;
    ux_key(sil, down);
}

// The iPad interface only reacts to touches (the desktop-era mouse handlers
// are still compiled in but the UI ignores them), so a mouse's left button and
// the gamepad's A button act as a finger.
#define SYNTH_TOUCH_ID 7777
static bool g_synth_down;
static void synth_touch(int phase, double sx, double sy) {
    if (phase == TOUCH_BEGAN) {
        if (g_synth_down) synth_touch(TOUCH_CANCELLED, sx, sy);
        g_synth_down = true;
    } else if (!g_synth_down) {
        return;
    }
    if (phase == TOUCH_ENDED || phase == TOUCH_CANCELLED) g_synth_down = false;
    double gx, gy;
    surface_to_game(sx, sy, &gx, &gy);
    fw_dispatch_touch(phase, SYNTH_TOUCH_ID, gx, gy, now_s());
}

// action: 0 move, 1 left down, 2 left up, 3 right down, 4 right up, 5 middle down, 6 middle up, 7 wheel
void ux_handle_mouse(int action, float sx, float sy, float wheel) {
    // Android may replay a resting pointer's hover; only real motion or a
    // button hands control from the gamepad cursor to the mouse.
    static float last_x = -1, last_y = -1;
    bool moved = fabsf(sx - last_x) > 2 || fabsf(sy - last_y) > 2;
    last_x = sx;
    last_y = sy;
    if (action != 0 || moved) cursor_hide();
    switch (action) {
    case 0: synth_touch(TOUCH_MOVED, sx, sy); break;  // drags while the button is down
    case 1: synth_touch(TOUCH_BEGAN, sx, sy); break;
    case 2: synth_touch(TOUCH_ENDED, sx, sy); break;
    case 3: ux_handle_back(); break;  // right button: close / back
    case 7: if (wheel != 0) zoom_at(wheel > 0 ? 1.25 : 0.8, sx, sy); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------------
// Gamepads. The game never acts on controller input, so a pad drives an
// on-screen cursor whose A button touches the screen:
//   left stick: move (d-pad: slow and precise)   A: tap / hold / drag   B: Back
//   X: pause   LB / RB: zoom out / in at the cursor   right stick: pan when zoomed
//   LT (hold): magnifier at the cursor   Start: Back (menu)   Select / R3: reset zoom
static struct {
    bool visible;
    double x, y;           // surface px
    float lx, ly, rx, ry;  // sticks after the dead zone, y down
    int hat_x, hat_y;      // d-pad reported as hat axes
    int keys[4];           // d-pad keys: up, down, left, right
    bool lt_axis, lt_key, loupe;
    double t_last;
} g_cur;

static void cursor_show(void) {
    present_rect d = present_get_dest();
    if (!g_cur.visible && g_cur.x == 0 && g_cur.y == 0) {
        g_cur.x = d.x + d.w / 2.0;
        g_cur.y = d.y + d.h / 2.0;
    }
    g_cur.visible = true;
    overlay_cursor(true, (float)g_cur.x, (float)g_cur.y);
}
static void cursor_hide(void) {
    if (!g_cur.visible) return;
    g_cur.visible = false;
    overlay_cursor(false, 0, 0);
    if (g_cur.loupe) overlay_loupe_touch(0, 0, 0);
    g_cur.loupe = false;
}

// Zoom by `factor`, keeping the game point under (sx, sy) in place.
static void zoom_at(double factor, double sx, double sy) {
    present_rect d = present_get_dest();
    rectd v = target_view();
    double z = view_zoom(v) * factor;
    if (z < 1.05) {
        ux_reset_zoom(true);
        return;
    }
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    double gx, gy;
    surface_to_game(sx, sy, &gx, &gy);
    double vw = g_display.render_w / z, vh = g_display.render_h / z;
    double x0 = gx - (sx - d.x) / d.w * vw, y0 = gy - (sy - d.y) / d.h * vh;
    set_view((rectd){x0, y0, x0 + vw, y0 + vh}, 0.15);
}
// Pan the zoomed view by (dx, dy) surface px.
static void pan_by(double dx, double dy) {
    present_rect d = present_get_dest();
    rectd v = target_view();
    if (view_zoom(v) < 1.01 || d.w <= 0) return;
    double k = (v.x1 - v.x0) / d.w;
    set_view((rectd){v.x0 + dx * k, v.y0 + dy * k, v.x1 + dx * k, v.y1 + dy * k}, 0);
}

static void loupe_update(void) {
    bool want = g_cur.visible && (g_cur.lt_axis || g_cur.lt_key);
    if (want && !g_cur.loupe) overlay_loupe_touch(3, (float)g_cur.x, (float)g_cur.y);
    if (!want && g_cur.loupe) overlay_loupe_touch(0, 0, 0);
    g_cur.loupe = want;
}

// Main thread, every run-loop pass: move the cursor, pan.
void ux_tick(void) {
    double t = now_s(), dt = t - g_cur.t_last;
    g_cur.t_last = t;
    if (dt > 0.1) dt = 0.1;
    if (!g_cur.visible) return;
    present_rect d = present_get_dest();
    double vx = 0, vy = 0, m = hypot(g_cur.lx, g_cur.ly);
    if (m > 0) {  // speed grows with tilt squared: fine control near the centre
        double sp = d.h * 1.3 * m * m;
        vx = g_cur.lx / m * sp;
        vy = g_cur.ly / m * sp;
    }
    int dx = g_cur.hat_x ? g_cur.hat_x : g_cur.keys[3] - g_cur.keys[2];
    int dy = g_cur.hat_y ? g_cur.hat_y : g_cur.keys[1] - g_cur.keys[0];
    vx += dx * d.h * 0.15;
    vy += dy * d.h * 0.15;
    if (vx != 0 || vy != 0) {
        double nx = g_cur.x + vx * dt, ny = g_cur.y + vy * dt;
        double x0 = d.x, y0 = d.y, x1 = d.x + d.w - 1, y1 = d.y + d.h - 1;
        // pushing past an edge while zoomed scrolls the view instead
        pan_by(nx < x0 ? nx - x0 : nx > x1 ? nx - x1 : 0, ny < y0 ? ny - y0 : ny > y1 ? ny - y1 : 0);
        g_cur.x = nx < x0 ? x0 : nx > x1 ? x1 : nx;
        g_cur.y = ny < y0 ? y0 : ny > y1 ? y1 : ny;
        overlay_cursor(true, (float)g_cur.x, (float)g_cur.y);
        synth_touch(TOUCH_MOVED, g_cur.x, g_cur.y);  // dragging with A held
        if (g_cur.loupe) overlay_loupe_touch(2, (float)g_cur.x, (float)g_cur.y);
    }
    if (g_cur.rx != 0 || g_cur.ry != 0) pan_by(g_cur.rx * d.h * 1.5 * dt, g_cur.ry * d.h * 1.5 * dt);
}

void ux_pad_connected(bool connected, const char *name) {
    LOGI("ux: gamepad %s%s%s", connected ? "connected" : "disconnected", name ? ": " : "", name ? name : "");
    if (!connected) {
        float keep_x = (float)g_cur.x, keep_y = (float)g_cur.y;
        cursor_hide();
        memset(&g_cur, 0, sizeof g_cur);
        g_cur.x = keep_x;
        g_cur.y = keep_y;
    }
}

// Android KEYCODE_BUTTON_* / DPAD_* from a gamepad
void ux_pad_button(int k, bool down) {
    switch (k) {
    case 96:  // A: touch at the cursor
        cursor_show();
        synth_touch(down ? TOUCH_BEGAN : TOUCH_ENDED, g_cur.x, g_cur.y);
        return;
    case 97:  // B: Back
        if (down) ux_handle_back();
        return;
    case 99:  // X: pause
        if (down) {
            int paused = toggle_pause();
            if (paused >= 0) overlay_flash(paused ? OVERLAY_ICON_PAUSE : OVERLAY_ICON_PLAY);
        }
        return;
    case 102: case 103:  // LB / RB: zoom
        if (down) {
            cursor_show();
            zoom_at(k == 103 ? 1.5 : 1 / 1.5, g_cur.x, g_cur.y);
        }
        return;
    case 104:  // L2 as a button: magnifier
        g_cur.lt_key = down;
        cursor_show();
        loupe_update();
        return;
    case 108:  // Start: Back (closes windows, game menu, quit prompt on the title screen)
        if (down) ux_handle_back();
        return;
    case 109: case 107:  // Select, right stick click: reset zoom
        if (down) ux_reset_zoom(true);
        return;
    case 19: case 20: case 21: case 22:  // d-pad: precise cursor movement
        g_cur.keys[k - 19] = down;
        if (down) cursor_show();
        return;
    default: return;
    }
}

static float deadzone(float v) {
    const float dz = 0.18f;
    if (fabsf(v) < dz) return 0;
    return (v - copysignf(dz, v)) / (1 - dz);
}
// axes: left x/y, right x/y, left/right trigger, hat x/y (Android: y down)
void ux_pad_axes(const float *ax) {
    g_cur.lx = deadzone(ax[0]);
    g_cur.ly = deadzone(ax[1]);
    g_cur.rx = deadzone(ax[2]);
    g_cur.ry = deadzone(ax[3]);
    g_cur.hat_x = ax[6] > 0.5f ? 1 : ax[6] < -0.5f ? -1 : 0;
    g_cur.hat_y = ax[7] > 0.5f ? 1 : ax[7] < -0.5f ? -1 : 0;
    g_cur.lt_axis = ax[4] > 0.5f;
    if (g_cur.lx != 0 || g_cur.ly != 0 || g_cur.hat_x || g_cur.hat_y || g_cur.lt_axis) cursor_show();
    loupe_update();
}

void ux_set_option(const char *key, float value) {
    LOGI("ux option %s = %g", key, value);
    if (!strcmp(key, "reset_zoom")) ux_reset_zoom(true);
    else if (!strcmp(key, "magnifier")) overlay_loupe_enable(value == 1 || (value == 0 && g_mag_auto));
    else if (!strcmp(key, "two_finger_pause")) g_two_finger_pause = value != 0;
    else if (!strcmp(key, "minimap")) overlay_minimap_enable(value != 0);
}

// ---------------------------------------------------------------------------------
// render-thread hook: view rect for present.c
void ux_present_view(double *x0, double *y0, double *x1, double *y1) {
    rectd v = current_view();
    if (v.x1 <= v.x0) v = full_view();
    *x0 = v.x0;
    *y0 = v.y0;
    *x1 = v.x1;
    *y1 = v.y1;
}
double ux_current_zoom(void) {
    rectd v = current_view();
    return v.x1 > v.x0 ? view_zoom(v) : 1.0;
}
