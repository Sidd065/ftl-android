// Phone UX layer for the FTL iPad build:
//  - screen layout / render resolution (render at a multiple of 768 px tall)
//  - zoomable view (pinch to zoom, two-finger pan) presented by present.c
//  - gestures: two-finger tap toggles pause; one finger held and a tap with another
//    is a right click for mods that listen for one; single touches go to the game
//  - Android Back -> Escape; hardware keyboard + mouse -> the game's own
//    (desktop-heritage) key and mouse events
//  - the beacon map and the upgrade screen zoomed in on phones (events too, as
//    an option); event text size; continuing the saved run at launch
//  - game logger hook for diagnostics; mods (mods.c)
#include "ux.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../fw/fw.h"
#include "../fw/present.h"
#include "../hyperspace.h"
#include "../hyperspace_host.h"
#include "../loader/macho.h"
#include "../patches.h"
#include "../platform/platform.h"
#include "../port_main.h"
#include "game.h"
#include "mods.h"
#include "overlay.h"
#include "png.h"
#include "targets.h"
#include "ux_internal.h"
#include "ux_present.h"
#include "port_log.h"

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
static bool g_haptics = true;
static int g_event_text;             // event text: 0 the game's size, 1 large, 2 larger
static int g_window_zoom;            // zoom in on the map and upgrades: 0 automatic (phones), 1 always, 2 off
static bool g_event_zoom;            // zoom in on events (off: it hides the HUD you may want to check)
static bool g_resume_run;            // continue the saved run at launch (a setting, off by default)
static int g_event_font_default;     // the game's own size (10; 9 in Japanese, which has 9 and 12 only)

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
    // The open/close-all-doors keys are tied to the door control buttons, which
    // the iPad build only shows in door mode. Accept them any time, like the
    // desktop game; the ship still checks that the door system is working.
    uint32_t *dk_open = mo_ptr(img, G_DOOR_KEY_OPEN), *dk_close = mo_ptr(img, G_DOOR_KEY_CLOSE);
    if (*dk_open == 0x34000228u && *dk_close == 0x34000228u) {  // cbz w8, ...
        *dk_open = *dk_close = 0xD503201Fu;                       // nop
        __builtin___clear_cache((char *)dk_open, (char *)(dk_close + 1));
    } else
        LOGW("ux: unexpected code in door hotkeys; Z/X only work in door mode");
    fails += png_install(img) != 0;
    if (targets_install(img) == 0) targets_set_frame_fn(pad_frame);
    if (mods_install(img, port_mods_index()) != 0) LOGW("ux: mods could not be loaded; playing without them");
    return fails ? -1 : 0;
}

// ---------------------------------------------------------------------------------
// game state probes
static void *capp(void) { return g_img ? *(void **)mo_ptr(g_img, G_CAPP_PTR) : NULL; }
uint8_t *ux_capp(void) { return capp(); }
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
    static const int subscreens[] = MENU_SUBSCREEN_OFFSETS;
    for (size_t i = 0; i < sizeof subscreens / sizeof *subscreens; i++)
        if (a[CAPP_MENU + subscreens[i]]) return false;
    return true;
}
// Toggle pause like the desktop hotkey. Returns the new state (-1: not in game).
int ux_toggle_pause(void) {
    uint8_t *a = capp();
    if (!a || a[CAPP_MENU] || a[CAPP_STARTUP]) return -1;
    uint8_t *gui = *(uint8_t **)(a + CAPP_GUI);
    if (!gui) return -1;
    ((void (*)(void *))mo_ptr(g_img, G_TOGGLE_PAUSE))(gui);
    return gui[GUI_PAUSED] != 0;
}
bool ux_paused(void) {
    uint8_t *a = capp();
    if (!a || a[CAPP_MENU] || a[CAPP_STARTUP]) return false;
    uint8_t *gui = *(uint8_t **)(a + CAPP_GUI);
    return gui && gui[GUI_PAUSED];
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
void ux_target_view(double v[4]) {
    rectd r = target_view();
    v[0] = r.x0, v[1] = r.y0, v[2] = r.x1, v[3] = r.y1;
}
void ux_reset_zoom(bool animate) { set_view(full_view(), animate ? 0.18 : 0); }

// Zoomed in on a window (see autozoom_tick)
static struct {
    bool active, player_moved;
    rectd before;         // the view to go back to
    double closed_since;  // the window has been shut since (0: it's open)
} g_az;
static void view_moved_by_player(void) {
    if (g_az.active) g_az.player_moved = true;
}

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
    // The game's HUD is 1024 units wide, so on screens narrower than 4:3 (foldables
    // opened up) the canvas would cut off its sides: use a 4:3 area, centred, with
    // bars above and below.
    if ((long)area_w * 3 < (long)area_h * 4) {
        int h = area_w * 3 / 4;
        t += (area_h - h) / 2;
        area_h = h;
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
static enum { G_IDLE, G_SINGLE, G_GESTURE, G_RIGHT } g_mode;
static int g_ga = -1, g_gb = -1;          // pointer slots used by the gesture
static int g_ra = -1, g_rb = -1;          // right click: the held finger, the one pressing the button
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
        int paused = g_two_finger_pause ? ux_toggle_pause() : -1;
        LOGI("ux: two-finger tap (%.0f ms) -> paused %d", dur * 1000, paused);
        if (paused >= 0) {
            overlay_flash(paused ? OVERLAY_ICON_PAUSE : OVERLAY_ICON_PLAY);
            ux_haptic(2);
        }
    } else {
        rectd v = target_view();
        view_moved_by_player();
        LOGI("ux: pinch/pan end, zoom %.2f (%.0f ms, moved %.0f px)", view_zoom(v), dur * 1000, g_gmove);
        if (view_zoom(v) < 1.08) ux_reset_zoom(true);
    }
    g_ga = g_gb = -1;
}

// Right clicks for mods (Hyperspace's scripts; the game itself has no use for them on the
// iPad layout): a finger held still, then a tap with another, clicks where the held finger
// is. Keeping the second finger down while moving the first drags. Only while a script
// listens for the right button; otherwise the second finger pinches or pauses as usual.
#define RIGHT_HOLD_S 0.25  // held at least this long; two-finger taps land closer together

static bool right_click_wanted(int held, double t) {
    ptr_t *p = &P[held];
    return p->to_game && t - p->t0 >= RIGHT_HOLD_S && hypot(p->sx - p->x0, p->sy - p->y0) < g_dpi / 6.0 &&
           hyperspace_wants_right_click();
}
static void right_button(int action) {
    double gx, gy;
    surface_to_game(P[g_ra].sx, P[g_ra].sy, &gx, &gy);
    hyperspace_right_button(action, gx, gy);
}
static void right_press(int held, int presser, double t) {
    if (P[held].to_game) {
        forward(TOUCH_CANCELLED, &P[held], t);  // the held finger stops being a touch
        P[held].to_game = false;
        overlay_loupe_touch(0, 0, 0);
    }
    g_mode = G_RIGHT;
    g_ra = held;
    g_rb = presser;
    right_button(HS_RIGHT_BUTTON_DOWN);
    ux_haptic(3);
    LOGI("ux: right click (finger held %.0f ms)", (t - P[held].t0) * 1000);
}

void ux_handle_touch(int phase, long id, float sx, float sy, double t) {
    if (phase == TOUCH_BEGAN) {
        pad_cursor_hide();
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
        } else if (g_mode == G_RIGHT) {
            // the first finger still down, the button up: another right click
            if (g_rb < 0 && g_ra >= 0) right_press(g_ra, s, t);
        } else if (g_mode != G_GESTURE) {
            // second finger: take over as a pinch/pan gesture
            int other = -1;
            for (int i = 0; i < MAXP; i++)
                if (P[i].active && i != s) other = i;
            if (other >= 0 && n == 2 && right_click_wanted(other, t)) {
                right_press(other, s, t);
                return;
            }
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
    if (g_mode == G_RIGHT) {
        if (phase == TOUCH_MOVED) {
            if (s == g_ra && g_rb >= 0) right_button(HS_RIGHT_BUTTON_MOVE);  // dragging
            return;
        }
        if (g_rb >= 0 && (s == g_ra || s == g_rb)) {
            right_button(HS_RIGHT_BUTTON_UP);
            g_rb = -1;
        }
        if (s == g_ra) g_ra = -1;
        P[s].active = false;
        if (active_count() == 0) g_mode = G_IDLE;
        return;
    }
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
    if (g_minimap_press && phase == TOUCH_ENDED && overlay_minimap_hit(sx, sy)) {
        view_moved_by_player();
        ux_reset_zoom(true);
    }
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
void ux_synth_touch_game(int phase, double gx, double gy) {
    if (phase == TOUCH_BEGAN) {
        if (g_synth_down) ux_synth_touch_game(TOUCH_CANCELLED, gx, gy);
        g_synth_down = true;
    } else if (!g_synth_down) {
        return;
    }
    if (phase == TOUCH_ENDED || phase == TOUCH_CANCELLED) g_synth_down = false;
    fw_dispatch_touch(phase, SYNTH_TOUCH_ID, gx, gy, now_s());
}
static void synth_touch(int phase, double sx, double sy) {
    double gx, gy;
    surface_to_game(sx, sy, &gx, &gy);
    ux_synth_touch_game(phase, gx, gy);
}

static bool g_mouse_right;
static void mouse_right(int action, double sx, double sy) {
    double gx, gy;
    surface_to_game(sx, sy, &gx, &gy);
    hyperspace_right_button(action, gx, gy);
}

// action: 0 move, 1 left down, 2 left up, 3 right down, 4 right up, 5 middle down, 6 middle up, 7 wheel
void ux_handle_mouse(int action, float sx, float sy, float wheel) {
    // Android may replay a resting pointer's hover; only real motion or a
    // button hands control from the gamepad cursor to the mouse.
    static float last_x = -1, last_y = -1;
    bool moved = fabsf(sx - last_x) > 2 || fabsf(sy - last_y) > 2;
    last_x = sx;
    last_y = sy;
    if (action != 0 || moved) pad_cursor_hide();
    switch (action) {
    case 0:
        synth_touch(TOUCH_MOVED, sx, sy);  // drags while the button is down
        if (g_mouse_right) mouse_right(HS_RIGHT_BUTTON_MOVE, sx, sy);
        break;
    case 1: synth_touch(TOUCH_BEGAN, sx, sy); break;
    case 2: synth_touch(TOUCH_ENDED, sx, sy); break;
    case 3:  // right button: a right click for mods that listen for one, otherwise close / back
        if (hyperspace_wants_right_click()) {
            g_mouse_right = true;
            mouse_right(HS_RIGHT_BUTTON_DOWN, sx, sy);
        } else {
            ux_handle_back();
        }
        break;
    case 4:
        if (g_mouse_right) mouse_right(HS_RIGHT_BUTTON_UP, sx, sy);
        g_mouse_right = false;
        break;
    case 7: if (wheel != 0) zoom_at(wheel > 0 ? 1.25 : 0.8, sx, sy); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------------
// zooming and panning (the mouse wheel, the gamepad)
// Zoom by `factor`, keeping the game point under (sx, sy) in place.
static void zoom_at(double factor, double sx, double sy) {
    present_rect d = present_get_dest();
    rectd v = target_view();
    view_moved_by_player();
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
    view_moved_by_player();
    double k = (v.x1 - v.x0) / d.w;
    set_view((rectd){v.x0 + dx * k, v.y0 + dy * k, v.x1 + dx * k, v.y1 + dy * k}, 0);
}

void ux_zoom_at(double factor, double sx, double sy) { zoom_at(factor, sx, sy); }
void ux_pan_by(double dx, double dy) { pan_by(dx, dy); }

// Keep the event window's text at the size picked in settings.
static void apply_event_font(void) {
    static int set;  // the size we last put there (0: the game's own is in use)
    uint8_t *a = capp();
    if (!a || a[CAPP_MENU] || a[CAPP_STARTUP]) return;
    uint8_t *gui = *(uint8_t **)(a + CAPP_GUI);
    if (!gui) return;
    int *size = (int *)(gui + GUI_CHOICE + CHOICE_FONT);
    if (!g_event_font_default) {
        if (*size < 9 || *size > 13) return;
        g_event_font_default = *size;
    }
    int want = g_event_text == 0 ? (set ? g_event_font_default : 0)
             : g_event_font_default < 10 || g_event_text == 1 ? 12 : 13;
    if (want && *size != want) {
        LOGI("ux: event text size %d -> %d", *size, want);
        *size = want;
    }
    set = g_event_text ? want : 0;
}

// What to zoom in on: the open window's useful part, in HUD units (the iPad's
// 1024-wide layout, which the game centres on our wider screen).
static bool window_rect(uint8_t *gui, rectd *r, bool *top) {
    // The game gives keys to the first open window in this list, so it runs
    // front to back: the menu comes before an event, an event before the store
    // (which can stay open underneath one).
    static const int modals[] = GUI_MODAL_OFFSETS;
    for (size_t i = 0; i < sizeof modals / sizeof *modals; i++)
        if (gui[modals[i] + 8]) return false;
    uint8_t *open = NULL;
    uint8_t **fw = (uint8_t **)(gui + GUI_FOCUS_WINDOWS);
    for (uint8_t **w = (uint8_t **)fw[0]; w < (uint8_t **)fw[1] && !open; w++)
        if (*w && (*w)[8]) open = *w;
    *top = false;
    if (open == gui + GUI_CHOICE) {
        // Event. ChoiceBox::OnRender puts the box at (199, 149), 150 further left when
        // not centred; the frame's outline reaches 7 units past its rect. The frame is
        // at least 377 tall, so short events leave it mostly empty: stop below the last
        // choice and keep the top in view.
        uint8_t *box = open;
        const int *frame = *(int **)(box + CHOICE_FRAME);
        if (!frame) return false;
        // Too many choices: the game uses a different, wider layout. Leave that one be.
        uint8_t **list = (uint8_t **)(box + CHOICE_LIST);
        if ((size_t)(list[1] - list[0]) / 584 > *(uint32_t *)(box + CHOICE_COLUMN)) return false;
        // The choices' rectangles are laid out once and then rewritten with the same
        // values each frame, so they're safe to read while the game draws.
        double x = (box[CHOICE_CENTRED] ? 199 : 49) + frame[0] - 7, y = 149 + frame[1] - 7;
        const int32_t *const *rects = (const int32_t *const *)(box + CHOICE_RECTS);
        double last = 0;
        for (const int32_t *c = rects[0]; c && c + 4 <= rects[1]; c += 4)
            if (c[3] > 0 && c[1] + c[3] > last) last = c[1] + c[3];
        if (last <= y) return false;  // not laid out yet
        double bottom = fmin(y + frame[3] + 14, last + 12);
        *r = (rectd){x, y, x + frame[2] + 14, bottom};
        *top = true;
        return frame[2] > 0;
    }
    if (open == gui + GUI_UPGRADES) {
        // Ship upgrades and cargo, with the description panel to the right. The crew
        // tab's dismissal box sits too low to gain much, so that tab stays as it is.
        if (*(uint32_t *)(open + 0x68) == 1) return false;
        *r = (rectd){46, 85, 977, 594};
        return true;
    }
    uint8_t *map = *(uint8_t **)(gui + GUI_STARMAP);
    if (open && open == map && !map[0x900]) {  // the beacon map, not the sector map
        int x = *(int *)(map + 0x88), y = *(int *)(map + 0x8c);
        *r = (rectd){x + 1, y + 1, x + 751, y + 580};
        return true;
    }
    return false;
}

// On phone-sized screens the beacon map and the upgrade screen get the screen to
// themselves, so their text is readable without pinching; events too, if the
// player wants. The view goes back once the window closes, unless the player
// moved it in between.
static void autozoom_tick(void) {
    uint8_t *a = capp();
    uint8_t *gui = a && !a[CAPP_MENU] && !a[CAPP_STARTUP] ? *(uint8_t **)(a + CAPP_GUI) : NULL;
    bool want_windows = g_window_zoom == 1 || (g_window_zoom == 0 && g_mag_auto);
    rectd r;
    bool top;  // set for events
    if (gui && window_rect(gui, &r, &top) && (top ? g_event_zoom : want_windows)) {
        g_az.closed_since = 0;
        if (g_az.player_moved || g_mode != G_IDLE) return;  // not while a finger is down
        // An event's text layout is only known once it has been drawn: wait for two equal readings.
        static rectd last;
        bool settled = !memcmp(&r, &last, sizeof r);
        last = r;
        if (!settled) return;
        double k = g_display.render_h / 768.0, hud_x = (g_display.render_w / k - 1024) / 2;
        r = (rectd){(r.x0 + hud_x) * k, r.y0 * k, (r.x1 + hud_x) * k, r.y1 * k};  // HUD units -> game px
        // Beyond about 2.2x the 2x render would look soft.
        double m = 14 * k;  // breathing room around the window
        double z = fmin(g_display.render_w / (r.x1 - r.x0 + 2 * m), g_display.render_h / (r.y1 - r.y0 + 2 * m));
        if (z > 2.2) z = 2.2;
        if (z < 1.15) return;
        double cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2, vw = g_display.render_w / z, vh = g_display.render_h / z;
        double y0 = top ? r.y0 - m : cy - vh / 2;
        rectd v = clamp_view((rectd){cx - vw / 2, y0, cx + vw / 2, y0 + vh});
        rectd cur = target_view();
        if (fabs(cur.x0 - v.x0) + fabs(cur.y0 - v.y0) + fabs(cur.x1 - v.x1) + fabs(cur.y1 - v.y1) < 2) return;
        if (!g_az.active) {
            g_az.active = true;
            g_az.before = cur;
            LOGI("ux: zooming in on a window (%.2fx)", z);
        }
        set_view(v, 0.25);
        overlay_minimap_hide(true);
        return;
    }
    if (!g_az.active) return;
    // Wait a moment before going back: one event often follows another.
    double t = now_s();
    if (!g_az.closed_since) g_az.closed_since = t;
    if (t - g_az.closed_since < 0.2 && gui && (g_event_zoom || want_windows)) return;
    if (!g_az.player_moved) set_view(g_az.before, 0.25);
    g_az.active = g_az.player_moved = false;
    g_az.closed_since = 0;
    overlay_minimap_hide(false);
}

// Android often ends the app while it's in the background, which on iOS was
// rare: come back to the run, not the title screen. Once, right after launch.
static void resume_run_tick(void) {
    static bool done;
    static double t_menu;
    uint8_t *a = capp();
    if (done || !a || a[CAPP_STARTUP]) return;  // the language chooser comes first
    if (!a[CAPP_MENU] || !at_top_menu()) {
        done = !a[CAPP_MENU];  // already playing
        t_menu = 0;
        return;
    }
    // The menu marks Continue active on its first frames: give it a moment.
    double t = now_s();
    if (!t_menu) t_menu = t;
    if (t - t_menu < 0.5) return;
    done = true;
    uint8_t *menu = a + CAPP_MENU;
    if (!g_resume_run || !menu[MENU_CONTINUE_ACTIVE]) return;
    LOGI("ux: continuing the saved run");
    *(int32_t *)(menu + MENU_ITEM) = 9;
}

// The title screen (or the first-launch language screen) has been up: the game is past
// loading, which with big mods can go a long while without drawing.
static bool g_started;
bool ux_game_started(void) { return __atomic_load_n(&g_started, __ATOMIC_RELAXED); }

// Main thread, every run-loop pass: resuming at launch, event text and window zoom, the
// gamepad cursor (pad.c).
void ux_tick(void) {
    uint8_t *a = capp();
    if (a && (a[CAPP_MENU] || a[CAPP_STARTUP])) __atomic_store_n(&g_started, true, __ATOMIC_RELAXED);
    resume_run_tick();
    apply_event_font();
    autozoom_tick();
    pad_tick();
}

void ux_set_option(const char *key, float value) {
    LOGI("ux option %s = %g", key, value);
    if (!strcmp(key, "reset_zoom")) ux_reset_zoom(true);
    else if (!strcmp(key, "magnifier")) overlay_loupe_enable(value == 1 || (value == 0 && g_mag_auto));
    else if (!strcmp(key, "two_finger_pause")) g_two_finger_pause = value != 0;
    else if (!strcmp(key, "minimap")) overlay_minimap_enable(value != 0);
    else if (!strcmp(key, "haptics")) g_haptics = value != 0;
    else if (!strcmp(key, "event_text")) g_event_text = (int)value;
    else if (!strcmp(key, "window_zoom")) g_window_zoom = (int)value;
    else if (!strcmp(key, "event_zoom")) g_event_zoom = value != 0;
    else if (!strcmp(key, "resume_run")) g_resume_run = value != 0;
    else if (!strncmp(key, "pad_", 4)) pad_set_option(key, value);
}

void ux_haptic(int kind) {
    if (g_haptics) plat_haptic(kind);
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
