// Game controllers. The game never acts on controller input, so a pad drives an
// on-screen cursor that touches the screen where it points. The cursor is kept in the
// game's own units (the HUD's 1024x768 layout), so it stays on what it points at when the
// view zooms or pans, crosses the same part of the game per second on any screen, and
// slows down when zoomed in; pushed against the edge of a zoomed view, it pans. Over
// clickable things it slows down, and once the stick is let go it drifts onto the one
// it's on (targets.c lists them).
//
// Buttons trigger actions, which the player can remap in settings. The defaults:
//   A: tap / hold / drag      B, Start: Back      X: pause
//   LB / RB: zoom out / in at the cursor          Select, R3: reset zoom
//   LT (hold): magnifier      RT: right click for mods (hold and move to drag)
// The left stick and the d-pad move the cursor (a d-pad press moves it a little, a held
// one slowly at first, then faster), the right stick pans a zoomed view. A DualSense or
// DualShock touchpad moves it like a laptop's: tap or press it to tap, with two fingers
// to right click.
#include "ux_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../fw/fw.h"
#include "../fw/present.h"
#include "../hyperspace.h"
#include "../hyperspace_host.h"
#include "../platform/platform.h"
#include "game.h"
#include "overlay.h"
#include "targets.h"
#include "ux.h"
#include "ux_present.h"
#include "port_log.h"

#define AT(T, p, off) (*(T *)((const uint8_t *)(p) + (off)))

// Actions. Settings store these numbers: add new ones at the end.
enum {
    ACT_NONE,
    ACT_TAP,          // touch at the cursor: tap, hold, drag
    ACT_RIGHT_CLICK,  // the right mouse button, for mods that use one
    ACT_BACK,         // close the window or open the game menu
    ACT_PAUSE,
    ACT_ZOOM_IN,
    ACT_ZOOM_OUT,
    ACT_RESET_ZOOM,
    ACT_MAGNIFIER,    // while held
    ACT_TURBO,        // while held: the cursor moves faster
    ACT_PRECISION,    // while held: slower, and not drawn to anything
    ACT_COUNT
};

// Android key codes
enum {
    K_DPAD_UP = 19, K_DPAD_DOWN = 20, K_DPAD_LEFT = 21, K_DPAD_RIGHT = 22,
    K_A = 96, K_B = 97, K_X = 99, K_L1 = 102, K_R1 = 103, K_L2 = 104, K_R2 = 105,
    K_R3 = 107, K_START = 108, K_SELECT = 109,
};

static uint8_t g_map[256] = {
    [K_A] = ACT_TAP, [K_B] = ACT_BACK, [K_START] = ACT_BACK, [K_X] = ACT_PAUSE,
    [K_L1] = ACT_ZOOM_OUT, [K_R1] = ACT_ZOOM_IN, [K_L2] = ACT_MAGNIFIER, [K_R2] = ACT_RIGHT_CLICK,
    [K_SELECT] = ACT_RESET_ZOOM, [K_R3] = ACT_RESET_ZOOM,
};

// Speeds in HUD units per second at no zoom
#define STICK_SPEED 1000.0  // full tilt
#define DPAD_STEP 2.0       // a press on its own
#define DPAD_SLOW 45.0      // held, for the first quarter second
#define DPAD_FAST 800.0     // held longer
#define TURBO 2.2
#define PRECISION 0.3
#define TOUCHPAD_GAIN 1.1   // the pad's width moves the cursor across this much of the view

static struct {
    bool present;          // a controller is connected
    bool visible, placed;
    double x, y;           // HUD units
    double sx, sy;         // where it was last drawn (surface px)
    float lx, ly, rx, ry;  // sticks after the dead zone, y down
    bool trig_axis[2];     // LT, RT pressed past half way, as an axis
    bool trig_key[2];      // ... or as a button
    int hat_x, hat_y;      // d-pad reported as hat axes
    bool dir_key[4];       // d-pad keys: up, down, left, right
    bool dir[4];           // held, as a key or on the hat
    double dir_t0[4];      // when each was pressed
    bool down[256];        // keys held (L2 and R2 also when pressed as triggers)
    int held[ACT_COUNT];   // keys held, per action
    bool tap, right, loupe;  // the finger, the right button and the magnifier are down
    double t_last;
} g_pad;

static bool g_magnetism = true, g_rumble = true, g_debug;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---------------------------------------------------------------------------------
// HUD units <-> game px <-> surface px
static double hud_k(void) { return g_display.render_h / 768.0; }
static double hud_left(void) { return (g_display.render_w / hud_k() - 1024) / 2; }  // canvas left of the HUD
static void hud_to_game(double hx, double hy, double *gx, double *gy) {
    double k = hud_k();
    *gx = (hx + hud_left()) * k;
    *gy = hy * k;
}
// The part of the game shown, in HUD units: x0, y0, x1, y1
static void visible_hud(double r[4]) {
    double v[4], k = hud_k(), l = hud_left();
    ux_target_view(v);
    r[0] = v[0] / k - l;
    r[1] = v[1] / k;
    r[2] = v[2] / k - l;
    r[3] = v[3] / k;
}
static double zoom_of(const double r[4]) { return r[3] > r[1] ? 768.0 / (r[3] - r[1]) : 1.0; }
static void hud_to_surface(double hx, double hy, double *sx, double *sy) {
    double gx, gy, x0, y0, x1, y1;
    hud_to_game(hx, hy, &gx, &gy);
    ux_present_view(&x0, &y0, &x1, &y1);  // as drawn: it may be animating
    present_rect d = present_get_dest();
    *sx = d.x + (gx - x0) / (x1 - x0) * d.w;
    *sy = d.y + (gy - y0) / (y1 - y0) * d.h;
}

// ---------------------------------------------------------------------------------
// the cursor
static void draw_cursor(void) {
    hud_to_surface(g_pad.x, g_pad.y, &g_pad.sx, &g_pad.sy);
    overlay_cursor(true, (float)g_pad.sx, (float)g_pad.sy);
    if (g_pad.loupe) overlay_loupe_touch(2, (float)g_pad.sx, (float)g_pad.sy);
}
static void cursor_show(void) {
    if (!g_pad.placed) {  // the first time: the middle of the view
        double r[4];
        visible_hud(r);
        g_pad.x = (r[0] + r[2]) / 2;
        g_pad.y = (r[1] + r[3]) / 2;
        g_pad.placed = true;
    }
    g_pad.visible = true;
    draw_cursor();
}
void pad_cursor_hide(void) {
    if (!g_pad.visible) return;
    g_pad.visible = false;
    overlay_cursor(false, 0, 0);
    if (g_pad.loupe) overlay_loupe_touch(0, 0, 0);
    g_pad.loupe = false;
}

static void finger(int phase) {
    double gx, gy;
    hud_to_game(g_pad.x, g_pad.y, &gx, &gy);
    ux_synth_touch_game(phase, gx, gy);
}
static void right_button(int action) {
    double gx, gy;
    hud_to_game(g_pad.x, g_pad.y, &gx, &gy);
    hyperspace_right_button(action, gx, gy);
}

// Move the cursor, keeping it on screen; pushed past the edge of a zoomed view, it pans
// (when the player is pushing it)
static void move_to(double nx, double ny, bool pan) {
    double r[4];
    visible_hud(r);
    double ox = nx < r[0] ? nx - r[0] : nx > r[2] ? nx - r[2] : 0;
    double oy = ny < r[1] ? ny - r[1] : ny > r[3] ? ny - r[3] : 0;
    if (pan && (ox != 0 || oy != 0) && zoom_of(r) > 1.01) {
        present_rect d = present_get_dest();
        double s = d.h / (r[3] - r[1]);  // surface px per HUD unit
        ux_pan_by(ox * s, oy * s);
        visible_hud(r);
    }
    double e = 0.01;
    nx = nx < r[0] ? r[0] : nx > r[2] - e ? r[2] - e : nx;
    ny = ny < r[1] ? r[1] : ny > r[3] - e ? r[3] - e : ny;
    if (nx == g_pad.x && ny == g_pad.y) return;
    g_pad.x = nx;
    g_pad.y = ny;
    if (g_pad.tap) finger(TOUCH_MOVED);  // dragging
    if (g_pad.right) right_button(HS_RIGHT_BUTTON_MOVE);
    draw_cursor();
}

// ---------------------------------------------------------------------------------
// magnetism
static ux_target g_targets[768];
static int g_ntargets;

// How much the cursor slows down over each kind of target while it's moved
static const double SLOWDOWN[] = {
    [TARGET_BUTTON] = 0.55, [TARGET_CHOICE] = 0.7, [TARGET_BEACON] = 0.5, [TARGET_TILE] = 0.8,
    [TARGET_DOOR] = 0.5, [TARGET_CREW] = 0.55, [TARGET_BOX] = 0.6,
};

// The target under the cursor (the smallest, if several), else the nearest within reach
static const ux_target *target_at(double x, double y, double reach, bool *inside) {
    const ux_target *best = NULL;
    double best_area = 1e18, best_dist = reach;
    *inside = false;
    for (int i = 0; i < g_ntargets; i++) {
        const ux_target *t = &g_targets[i];
        double dx = fmax(fmax(t->x - x, x - (t->x + t->w)), 0), dy = fmax(fmax(t->y - y, y - (t->y + t->h)), 0);
        double dist = hypot(dx, dy), area = (double)t->w * t->h;
        if (dist == 0) {
            if (!*inside || area < best_area) best = t, best_area = area;
            *inside = true;
        } else if (!*inside && dist < best_dist) {
            best = t, best_dist = dist;
        }
    }
    return best;
}

// Where a target draws the cursor to: its middle, or for a long one (an event choice,
// a wide button) the nearest point along its middle
static void snap_point(const ux_target *t, double x, double y, double *px, double *py) {
    double ix = fmin(t->w / 2, 16), iy = fmin(t->h / 2, 16);
    *px = fmin(fmax(x, t->x + ix), t->x + t->w - ix);
    *py = fmin(fmax(y, t->y + iy), t->y + t->h - iy);
}

// ---------------------------------------------------------------------------------
// actions
static void loupe_update(void) {
    bool want = g_pad.visible && g_pad.held[ACT_MAGNIFIER] > 0;
    if (want && !g_pad.loupe) overlay_loupe_touch(3, (float)g_pad.sx, (float)g_pad.sy);
    if (!want && g_pad.loupe) overlay_loupe_touch(0, 0, 0);
    g_pad.loupe = want;
}

static void action(int act, bool down) {
    switch (act) {
    case ACT_TAP:
        cursor_show();
        g_pad.tap = down;
        finger(down ? TOUCH_BEGAN : TOUCH_ENDED);
        break;
    case ACT_RIGHT_CLICK:
        if (down && !g_pad.right && hyperspace_wants_right_click()) {
            cursor_show();
            g_pad.right = true;
            right_button(HS_RIGHT_BUTTON_DOWN);
            ux_haptic(3);
            LOGI("ux: right click from the controller");
        } else if (!down && g_pad.right) {
            g_pad.right = false;
            right_button(HS_RIGHT_BUTTON_UP);
        }
        break;
    case ACT_BACK:
        if (down) ux_handle_back();
        break;
    case ACT_PAUSE:
        if (down) {
            int paused = ux_toggle_pause();
            if (paused >= 0) overlay_flash(paused ? OVERLAY_ICON_PAUSE : OVERLAY_ICON_PLAY);
        }
        break;
    case ACT_ZOOM_IN:
    case ACT_ZOOM_OUT:
        if (down) {
            cursor_show();
            ux_zoom_at(act == ACT_ZOOM_IN ? 1.5 : 1 / 1.5, g_pad.sx, g_pad.sy);
        }
        break;
    case ACT_RESET_ZOOM:
        if (down) ux_reset_zoom(true);
        break;
    case ACT_MAGNIFIER:
        cursor_show();
        loupe_update();
        break;
    default:  // turbo, precision: while held (pad_tick)
        break;
    }
}

static void key_change(int code, bool down) {
    if (code < 0 || code > 255 || g_pad.down[code] == down) return;
    g_pad.down[code] = down;
    int act = g_map[code];
    if (act <= ACT_NONE || act >= ACT_COUNT) return;
    bool was = g_pad.held[act] > 0;
    g_pad.held[act] += down ? 1 : -1;
    if (g_pad.held[act] < 0) g_pad.held[act] = 0;
    if (was != (g_pad.held[act] > 0)) action(act, !was);
}

// Let go of everything held (the controller went, or the buttons were remapped)
static void release_all(void) {
    for (int k = 0; k < 256; k++)
        if (g_pad.down[k]) key_change(k, false);
    memset(g_pad.trig_axis, 0, sizeof g_pad.trig_axis);
    memset(g_pad.trig_key, 0, sizeof g_pad.trig_key);
    memset(g_pad.dir_key, 0, sizeof g_pad.dir_key);
    memset(g_pad.dir, 0, sizeof g_pad.dir);
    g_pad.hat_x = g_pad.hat_y = 0;
    g_pad.lx = g_pad.ly = g_pad.rx = g_pad.ry = 0;
}

static void trigger_change(int i) {
    key_change(i ? K_R2 : K_L2, g_pad.trig_axis[i] || g_pad.trig_key[i]);
}

static double dpad_speed(double held) {
    if (held < 0.25) return DPAD_SLOW;
    double s = fmin(1, (held - 0.25) / 0.7);
    return DPAD_SLOW + (DPAD_FAST - DPAD_SLOW) * s * s;
}

static const int DIR_X[4] = {0, 0, -1, 1}, DIR_Y[4] = {-1, 1, 0, 0};

static void update_dir(int i) {
    bool hat = i == 0 ? g_pad.hat_y < 0 : i == 1 ? g_pad.hat_y > 0 : i == 2 ? g_pad.hat_x < 0 : g_pad.hat_x > 0;
    bool on = g_pad.dir_key[i] || hat;
    if (on == g_pad.dir[i]) return;
    g_pad.dir[i] = on;
    if (!on) return;
    g_pad.dir_t0[i] = now_s();
    bool shown = g_pad.visible;
    cursor_show();
    if (!shown) return;  // the first press only brings the cursor back
    double r[4];
    visible_hud(r);
    double step = DPAD_STEP / zoom_of(r);
    move_to(g_pad.x + DIR_X[i] * step, g_pad.y + DIR_Y[i] * step, true);
}

// ---------------------------------------------------------------------------------
// input from the platform (main thread)
void ux_pad_connected(bool connected, const char *name) {
    LOGI("ux: gamepad %s%s%s", connected ? "connected" : "disconnected", name ? ": " : "", name ? name : "");
    g_pad.present = connected;
    if (connected) return;
    release_all();
    pad_cursor_hide();
    // like on a console: a run stops when its controller goes
    if (!ux_paused() && ux_toggle_pause() == 1) overlay_flash(OVERLAY_ICON_PAUSE);
}

// Android KEYCODE_BUTTON_* / DPAD_* from a gamepad
void ux_pad_button(int k, bool down) {
    if (k >= K_DPAD_UP && k <= K_DPAD_RIGHT) {
        g_pad.dir_key[k - K_DPAD_UP] = down;
        update_dir(k - K_DPAD_UP);
    } else if (k == K_L2 || k == K_R2) {
        g_pad.trig_key[k == K_R2] = down;
        trigger_change(k == K_R2);
    } else {
        key_change(k, down);
    }
}

static float deadzone(float v) {
    const float dz = 0.18f;
    if (fabsf(v) < dz) return 0;
    return (v - copysignf(dz, v)) / (1 - dz);
}

// axes: left x/y, right x/y, left/right trigger, hat x/y (Android: y down)
void ux_pad_axes(const float *ax) {
    g_pad.lx = deadzone(ax[0]);
    g_pad.ly = deadzone(ax[1]);
    g_pad.rx = deadzone(ax[2]);
    g_pad.ry = deadzone(ax[3]);
    for (int i = 0; i < 2; i++) {
        bool on = g_pad.trig_axis[i] ? ax[4 + i] > 0.35f : ax[4 + i] > 0.55f;
        if (on != g_pad.trig_axis[i]) {
            g_pad.trig_axis[i] = on;
            trigger_change(i);
        }
    }
    g_pad.hat_x = ax[6] > 0.5f ? 1 : ax[6] < -0.5f ? -1 : 0;
    g_pad.hat_y = ax[7] > 0.5f ? 1 : ax[7] < -0.5f ? -1 : 0;
    for (int i = 0; i < 4; i++) update_dir(i);
    if (g_pad.lx != 0 || g_pad.ly != 0) cursor_show();
}

// A DualSense or DualShock touchpad: fingers on it (0 when lifted), the first one's
// position (0..1 across and down), whether it's pressed in
static struct {
    bool down, pressed, clicked;
    int fingers, most;     // on it now, and the most during this touch
    float x, y, travel;
    double t0;
    int click;             // what pressing it did: ACT_TAP or ACT_RIGHT_CLICK
} g_tp;

static int touchpad_click_kind(int fingers) {
    return fingers >= 2 && hyperspace_wants_right_click() ? ACT_RIGHT_CLICK : ACT_TAP;
}

void ux_pad_trackpad(int fingers, float x, float y, bool pressed) {
    double t = now_s();
    if (fingers > 0 && !g_tp.down) {
        g_tp.down = true;
        g_tp.fingers = g_tp.most = fingers;
        g_tp.x = x;
        g_tp.y = y;
        g_tp.t0 = t;
        g_tp.travel = 0;
        cursor_show();
    } else if (fingers > 0) {
        if (fingers == g_tp.fingers) {  // (a finger landing or lifting makes the first one's position jump)
            float dx = x - g_tp.x, dy = y - g_tp.y;
            double r[4];
            visible_hud(r);
            g_tp.travel += hypotf(dx, dy);
            move_to(g_pad.x + dx * (r[2] - r[0]) * TOUCHPAD_GAIN, g_pad.y + dy * (r[3] - r[1]) * TOUCHPAD_GAIN, true);
        }
        g_tp.x = x;
        g_tp.y = y;
        g_tp.fingers = fingers;
        if (fingers > g_tp.most) g_tp.most = fingers;
    }
    if (pressed != g_tp.pressed) {  // pressed in: a tap, or a right click with two fingers; held, it drags
        g_tp.pressed = pressed;
        if (pressed) {
            g_tp.click = touchpad_click_kind(fingers);
            g_tp.clicked = true;
        }
        action(g_tp.click, pressed);
    }
    if (fingers == 0 && g_tp.down) {
        g_tp.down = false;
        g_tp.fingers = 0;
        // a quick touch that didn't move, without pressing in: a tap
        if (!g_tp.clicked && t - g_tp.t0 < 0.25 && g_tp.travel < 0.02f) {
            int act = touchpad_click_kind(g_tp.most);
            action(act, true);
            action(act, false);
        }
        g_tp.clicked = false;
    }
}

// ---------------------------------------------------------------------------------
// each run-loop pass (main thread)
static void debug_draw_targets(void) {
    // buttons green, choices cyan, beacons magenta, tiles grey, doors orange, crew yellow, boxes blue
    static const unsigned COLORS[] = {0x33ff66, 0x33ffff, 0xff44ff, 0x8899aa, 0xff9922, 0xffee33, 0x4488ff};
    static float rects[768 * 4];
    static unsigned colors[768];
    int n = 0;
    for (int i = 0; i < g_ntargets && n < 768; i++, n++) {
        colors[n] = COLORS[g_targets[i].kind % (sizeof COLORS / sizeof *COLORS)];
        double x0, y0, x1, y1;
        hud_to_surface(g_targets[i].x, g_targets[i].y, &x0, &y0);
        hud_to_surface(g_targets[i].x + g_targets[i].w, g_targets[i].y + g_targets[i].h, &x1, &y1);
        rects[n * 4] = (float)x0, rects[n * 4 + 1] = (float)y0;
        rects[n * 4 + 2] = (float)(x1 - x0), rects[n * 4 + 3] = (float)(y1 - y0);
    }
    overlay_debug_rects(rects, colors, n);
}

void pad_tick(void) {
    double t = now_s(), dt = t - g_pad.t_last;
    g_pad.t_last = t;
    if (dt > 0.1) dt = 0.1;
    if (g_magnetism || g_debug) g_ntargets = targets_get(g_targets, 768);
    if (g_debug) debug_draw_targets();
    if (!g_pad.visible) return;

    double r[4];
    visible_hud(r);
    double zoom = zoom_of(r);
    // the speed asked for, in HUD units per second
    double vx = 0, vy = 0, tilt = hypot(g_pad.lx, g_pad.ly);
    if (tilt > 0) {  // speed grows with tilt squared: fine control near the centre
        vx = g_pad.lx * STICK_SPEED * tilt;
        vy = g_pad.ly * STICK_SPEED * tilt;
    }
    bool dpad = false;
    for (int i = 0; i < 4; i++)
        if (g_pad.dir[i]) {
            double s = dpad_speed(t - g_pad.dir_t0[i]);
            vx += DIR_X[i] * s;
            vy += DIR_Y[i] * s;
            dpad = true;
        }
    double input = dpad ? 1 : fmin(1, tilt);
    bool precise = g_pad.held[ACT_PRECISION] > 0;
    double mult = 1 / zoom * (g_pad.held[ACT_TURBO] > 0 ? TURBO : 1) * (precise ? PRECISION : 1);

    double nx = g_pad.x, ny = g_pad.y;
    bool inside = false;
    const ux_target *tg = g_magnetism && !precise ? target_at(g_pad.x, g_pad.y, 14 / zoom, &inside) : NULL;
    if (tg && inside && input > 0) mult *= SLOWDOWN[tg->kind];
    if (tg && input < 0.35 && !g_pad.tap && !g_pad.right && !g_tp.down) {  // let go: drift onto it
        double px, py;
        snap_point(tg, g_pad.x, g_pad.y, &px, &py);
        double a = 1 - exp(-dt * 14 * (1 - input / 0.35));
        if (hypot(px - nx, py - ny) > 0.05) {
            nx += (px - nx) * a;
            ny += (py - ny) * a;
        }
    }
    nx += vx * mult * dt;
    ny += vy * mult * dt;
    // (when the view zooms in on a window, away from the cursor, it comes back on screen at
    // the nearest point; only pushing it pans)
    if (nx != g_pad.x || ny != g_pad.y || nx < r[0] || nx > r[2] || ny < r[1] || ny > r[3])
        move_to(nx, ny, vx != 0 || vy != 0);
    else draw_cursor();  // the view may have moved under it

    if (g_pad.rx != 0 || g_pad.ry != 0) {
        present_rect d = present_get_dest();
        ux_pan_by(g_pad.rx * d.h * 1.5 * dt, g_pad.ry * d.h * 1.5 * dt);
    }
}

void pad_set_option(const char *key, float value) {
    if (!strcmp(key, "pad_magnetism")) g_magnetism = value != 0;
    else if (!strcmp(key, "pad_rumble")) g_rumble = value != 0;
    else if (!strcmp(key, "pad_debug")) {
        g_debug = value != 0;
        if (!g_debug) overlay_debug_rects(NULL, NULL, 0);
    } else if (!strcmp(key, "pad_map_reset")) {
        release_all();
        memset(g_map, 0, sizeof g_map);
    } else if (!strncmp(key, "pad_btn_", 8)) {
        int code = atoi(key + 8);
        if (code > 0 && code < 256 && value >= 0 && value < ACT_COUNT) g_map[code] = (uint8_t)value;
    }
}

// ---------------------------------------------------------------------------------
// rumble: the game's thread, at the start of each frame
static struct {
    const void *ship;
    int hull, damage;
    bool jumping;
    bool queued[8];
} g_rs;

static int vec_count(const void *vec, size_t elem) {
    const uint8_t *b = AT(const uint8_t *, vec, 0), *e = AT(const uint8_t *, vec, 8);
    return b && e > b ? (int)((size_t)(e - b) / elem) : 0;
}

void pad_frame(void *app_) {
    const uint8_t *app = app_, *sm = NULL;
    if (g_rumble && g_pad.present && app && !app[CAPP_MENU] && !app[CAPP_STARTUP]) {
        const uint8_t *gui = AT(const uint8_t *, app, CAPP_GUI);
        const uint8_t *ship = gui ? AT(const uint8_t *, gui, GUI_SHIP) : NULL;
        sm = ship ? AT(const uint8_t *, ship, COMPLETE_SHIP_MANAGER) : NULL;
    }
    if (!sm) {
        g_rs.ship = NULL;
        return;
    }
    int hull = AT(int32_t, sm, SM_SHIP + SHIP_HULL), damage = 0;
    int n = vec_count(sm + SM_SYSTEMS, 8);
    const uint8_t *const *systems = AT(const uint8_t *const *, sm, SM_SYSTEMS);
    for (int i = 0; i < n && i < 32; i++) {
        if (!systems[i]) continue;
        int health = AT(int32_t, systems[i], SYSTEM_HEALTH), max = AT(int32_t, systems[i], SYSTEM_HEALTH + 4);
        if (max > 0 && max <= 16 && health >= 0 && health <= max) damage += max - health;
    }
    bool jumping = AT(bool, sm, SM_JUMPING);
    bool queued[8] = {0};
    const uint8_t *weapons = AT(const uint8_t *, sm, SM_WEAPONS);
    if (weapons) {
        n = vec_count(weapons + WEAPONS_LIST, 8);
        const uint8_t *const *list = AT(const uint8_t *const *, weapons, WEAPONS_LIST);
        for (int i = 0; i < n && i < 8; i++)
            queued[i] = list[i] && vec_count(list[i] + WEAPON_QUEUED, 8) > 0;
    }
    if (sm == g_rs.ship) {
        float strength = 0;
        int ms = 0;
        if (hull < g_rs.hull) {  // the hull was hit
            strength = fminf(1, 0.45f + 0.15f * (float)(g_rs.hull - hull));
            ms = 260;
        } else if (damage > g_rs.damage) {  // a system was
            strength = 0.35f;
            ms = 150;
        }
        if (jumping && !g_rs.jumping) {
            strength = fmaxf(strength, 0.5f);
            ms = 600;
        }
        for (int i = 0; i < 8; i++)
            if (queued[i] && !g_rs.queued[i] && strength < 0.2f) {  // a weapon fired
                strength = 0.2f;
                ms = 70;
            }
        if (strength > 0) plat_pad_rumble(strength, ms);
    }
    g_rs.ship = sm;
    g_rs.hull = hull;
    g_rs.damage = damage;
    g_rs.jumping = jumping;
    memcpy(g_rs.queued, queued, sizeof queued);
}
