// Clickable things for the gamepad cursor (see targets.h). The game's thread fills a list
// while it draws a frame: buttons as they're drawn (Button::OnRender and
// TextButton::OnRender, which Hyperspace's buttons and mods' Lua buttons go through
// too), the rest from the game's state at the start of the frame. The list is published
// at the start of the next frame, so it also has what Hyperspace draws after the game's
// own drawing. A window that takes all input (a question, the game menu, the map, a
// store...) drops the buttons drawn before it, which it covers.
#include "targets.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "../fw/fw.h"
#include "../loader/macho.h"
#include "../patches.h"
#include "game.h"
#include "port_log.h"

#define MAX_TARGETS 768
#define AT(T, p, off) (*(T *)((const uint8_t *)(p) + (off)))

static ux_target g_build[MAX_TARGETS], g_state[MAX_TARGETS], g_pub[MAX_TARGETS];
static int g_nbuild, g_nstate, g_npub;
static int g_barrier = -1;  // where the top window began drawing this frame
static int g_window_depth;  // windows being drawn, one inside the other
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*g_frame_fn)(void *app);
static void (*g_capp_render)(void *app);
static void (*g_button_render)(void *button);
static void (*g_textbutton_render)(void *button);

static void add_to(ux_target *list, int *n, float x, float y, float w, float h, int kind) {
    if (*n >= MAX_TARGETS || w <= 0 || h <= 0) return;
    list[(*n)++] = (ux_target){x, y, w, h, (uint8_t)kind};
}
// from the game's state
static void add(float x, float y, float w, float h, int kind) { add_to(g_state, &g_nstate, x, y, w, h, kind); }

// libc++ std::vector<T *>: begin, end, capacity
static int vec_ptrs(const void *vec, void ***items, int max) {
    void **b = AT(void **, vec, 0), **e = AT(void **, vec, 8);
    if (!b || e < b) return 0;
    *items = b;
    return e - b > max ? max : (int)(e - b);
}

static void add_rect(const void *rect, int kind) {
    const int32_t *r = rect;
    add((float)r[0], (float)r[1], (float)r[2], (float)r[3], kind);
}

static const float *g_matrix;  // the game's current 4x4 drawing matrix (CSurface)
static float g_hud_left;       // its x translation when drawing the HUD itself (centring it)
static const uint8_t *g_app;   // the CApp being drawn

// A button drawn with the matrix moved (inside a window drawn at an offset, like the map)
// is drawn, and touched, that far from its hitbox
static void add_button(const void *button) {
    if (!AT(bool, button, BUTTON_ACTIVE)) return;
    const int32_t *r = (const int32_t *)((const uint8_t *)button + BUTTON_HITBOX);
    float dx = g_matrix ? g_matrix[12] - g_hud_left : 0, dy = g_matrix ? g_matrix[13] : 0;
    add_to(g_build, &g_nbuild, r[0] + dx, r[1] + dy, (float)r[2], (float)r[3], TARGET_BUTTON);
}
static void button_render_hook(void *button) {
    add_button(button);
    g_button_render(button);
}
static void textbutton_render_hook(void *button) {
    add_button(button);
    g_textbutton_render(button);
}

static const uint8_t *ship_manager(const void *complete_ship) {
    return complete_ship ? AT(const uint8_t *, complete_ship, COMPLETE_SHIP_MANAGER) : NULL;
}

// Ship coordinates -> HUD units, through the matrix the ship is being drawn with (the
// iPad shrinks the player's ship in a fight, and as it jumps)
static void ship_point(float x, float y, float *hx, float *hy) {
    const float *m = g_matrix;
    *hx = m[0] * x + m[4] * y + m[12] - g_hud_left;
    *hy = m[1] * x + m[5] * y + m[13];
}
static void add_ship_rect(float x, float y, float w, float h, int kind) {
    float x0, y0, x1, y1;
    ship_point(x, y, &x0, &y0);
    ship_point(x + w, y + h, &x1, &y1);
    add_to(g_build, &g_nbuild, fminf(x0, x1), fminf(y0, y1), fabsf(x1 - x0), fabsf(y1 - y0), kind);
}

// As a ship is drawn: its floor tiles, its doors while the door button is on (the
// player's), and the player's crew aboard it
static void add_ship(const uint8_t *sm) {
    const uint8_t *app = g_app;
    if (!g_matrix || !app || app[CAPP_MENU] || app[CAPP_STARTUP]) return;  // the hangar's ship: nothing to tap
    const uint8_t *gui = AT(const uint8_t *, app, CAPP_GUI);
    if (!gui) return;
    int id = AT(int32_t, sm, SM_ID);
    const uint8_t *ship = sm + SM_SHIP;
    void **items;
    int n = vec_ptrs(ship + SHIP_ROOMS, &items, 64);
    for (int i = 0; i < n; i++) {
        if (!items[i]) continue;
        const int32_t *r = (const int32_t *)((const uint8_t *)items[i] + ROOM_RECT);
        for (int ty = 0; ty < r[3] / 35 && ty < 8; ty++)
            for (int tx = 0; tx < r[2] / 35 && tx < 8; tx++)
                add_ship_rect(r[0] + tx * 35, r[1] + ty * 35, 35, 35, TARGET_TILE);
    }
    if (id == 0 && gui[GUI_CREW_CONTROL + CREWCTL_DOOR_MODE]) {
        n = vec_ptrs(ship + SHIP_DOORS, &items, 128);
        for (int i = 0; i < n; i++) {
            if (!items[i]) continue;
            float x = AT(int32_t, items[i], DOOR_X), y = AT(int32_t, items[i], DOOR_Y);
            if (AT(bool, items[i], DOOR_VERTICAL)) add_ship_rect(x - 7, y - 17, 14, 35, TARGET_DOOR);
            else add_ship_rect(x - 17, y - 7, 35, 14, TARGET_DOOR);
        }
    }
    const uint8_t *player = ship_manager(AT(const void *, gui, GUI_SHIP));
    n = player ? vec_ptrs(player + SM_CREW, &items, 64) : 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *c = items[i];
        if (c && !AT(bool, c, CREW_DEAD) && AT(int32_t, c, CREW_SHIP) == id)
            add_ship_rect(AT(float, c, CREW_X) - 11, AT(float, c, CREW_Y) - 14, 22, 28, TARGET_CREW);
    }
}

static void collect(const uint8_t *app) {
    if (!app || app[CAPP_MENU] || app[CAPP_STARTUP]) return;  // the title screen: its buttons
    const uint8_t *gui = AT(const uint8_t *, app, CAPP_GUI);
    if (!gui) return;
    static const int modals[] = GUI_MODAL_OFFSETS;
    for (size_t i = 0; i < sizeof modals / sizeof *modals; i++)
        if (gui[modals[i] + 8]) return;  // the game menu or a question: its buttons
    if (gui[GUI_CHOICE + 8]) {  // an event: its choices
        const int32_t *b = AT(const int32_t *, gui, GUI_CHOICE + CHOICE_RECTS);
        const int32_t *e = AT(const int32_t *, gui, GUI_CHOICE + CHOICE_RECTS + 8);
        for (const int32_t *c = b; c && c + 4 <= e && c < b + 4 * 32; c += 4) add_rect(c, TARGET_CHOICE);
        return;
    }
    void **items;
    const uint8_t *map = AT(const uint8_t *, gui, GUI_STARMAP);
    if (map && map[8]) {  // the beacon map
        if (map[MAP_NEW_SECTOR]) return;
        float mx = AT(int32_t, map, MAP_POSITION) + AT(float, map, MAP_TRANSLATION);
        float my = AT(int32_t, map, MAP_POSITION + 4) + AT(float, map, MAP_TRANSLATION + 4);
        int n = vec_ptrs(map + MAP_LOCATIONS, &items, 128);
        for (int i = 0; i < n; i++)
            if (items[i]) add(mx + AT(float, items[i], 0) - 12, my + AT(float, items[i], 4) - 12, 24, 24, TARGET_BEACON);
        return;
    }
    int n = vec_ptrs(gui + GUI_FOCUS_WINDOWS, &items, 32);
    for (int i = 0; i < n; i++)
        if (items[i] && ((const uint8_t *)items[i])[8]) return;  // a store, upgrades...: their buttons

    // The HUD
    n = vec_ptrs(gui + GUI_CREW_CONTROL + CREWCTL_BOXES, &items, 64);
    for (int i = 0; i < n; i++)
        if (items[i]) add_rect(items[i], TARGET_BOX);
    n = vec_ptrs(gui + GUI_SYS_CONTROL + SYSCTL_BOXES, &items, 32);
    int sx = AT(int32_t, gui, GUI_SYS_CONTROL + SYSCTL_POSITION), sy = AT(int32_t, gui, GUI_SYS_CONTROL + SYSCTL_POSITION + 4);
    for (int i = 0; i < n; i++) {
        if (!items[i]) continue;
        const int32_t *r = (const int32_t *)((const uint8_t *)items[i] + SYSBOX_HITBOX);
        add(sx + r[0], sy + r[1], r[2], r[3], TARGET_BOX);
    }
    if (AT(bool, gui, GUI_FTL_BUTTON + BUTTON_ACTIVE)) add_rect(gui + GUI_FTL_BUTTON + BUTTON_HITBOX, TARGET_BUTTON);
    // Weapons and drones: a column of small boxes, or the big panel a tap on it opens
    static const int armaments[] = {GUI_WEAPON_CONTROL, GUI_DRONE_CONTROL};
    for (size_t a = 0; a < sizeof armaments / sizeof *armaments; a++) {
        const uint8_t *ac = gui + armaments[a];
        int ax = AT(int32_t, ac, ARMCTL_LOCATION), ay = AT(int32_t, ac, ARMCTL_LOCATION + 4);
        int aw = AT(int32_t, ac, ARMCTL_PANEL + 8);
        bool open = AT(bool, ac, ARMCTL_OPEN);
        n = vec_ptrs(ac + ARMCTL_BOXES, &items, 8);
        for (int i = 0; i < n; i++) {
            const uint8_t *b = items[i];
            if (!b || AT(int32_t, b, ARMBOX_ICON) == INT32_MAX) continue;  // never drawn: no such system
            if (open)
                add(ax + AT(int32_t, b, ARMBOX_LOCATION), ay + AT(int32_t, b, ARMBOX_LOCATION + 4), aw, 78, TARGET_BOX);
            else
                add(AT(int32_t, b, ARMBOX_ICON) - 3, AT(int32_t, b, ARMBOX_ICON + 4) - 3, 32, 35, TARGET_BOX);
        }
    }
    // the ships: as they're drawn (ship_render_hook)
}

static void capp_render_hook(void *app) {
    int from = g_barrier >= 0 ? g_barrier : 0, n = g_nbuild - from;
    pthread_mutex_lock(&g_lock);
    memcpy(g_pub, g_build + from, (size_t)n * sizeof *g_build);
    int extra = g_nstate < MAX_TARGETS - n ? g_nstate : MAX_TARGETS - n;
    memcpy(g_pub + n, g_state, (size_t)extra * sizeof *g_state);
    g_npub = n + extra;
    pthread_mutex_unlock(&g_lock);
    g_nbuild = g_nstate = 0;
    g_barrier = -1;
    g_window_depth = 0;
    double k = g_display.render_h / 768.0;
    g_hud_left = k > 0 ? (float)((g_display.render_w / k - 1024) / 2) : 0;
    g_app = app;
    collect(app);
    if (g_frame_fn) g_frame_fn(app);
    g_capp_render(app);
}

// Windows that take all input while open (FocusWindow: open flag at +8)
#define WINDOW_HOOK(name)                                                     \
    static void (*g_##name##_render)(void *);                               \
    static void name##_render_hook(void *w) {                                 \
        if (((const uint8_t *)w)[8] && g_window_depth == 0) g_barrier = g_nbuild; \
        g_window_depth++;                                                     \
        g_##name##_render(w);                                                 \
        g_window_depth--;                                                     \
    }
WINDOW_HOOK(confirm)
WINDOW_HOOK(menu)
WINDOW_HOOK(options)
WINDOW_HOOK(choice)
WINDOW_HOOK(language)
WINDOW_HOOK(starmap)
WINDOW_HOOK(store)
WINDOW_HOOK(tabbed)
WINDOW_HOOK(input)
WINDOW_HOOK(gameover)

static const struct {
    uint64_t addr;
    void *hook;
    void (**orig)(void *);
} WINDOWS[] = {
    {G_CONFIRM_ONRENDER, (void *)confirm_render_hook, &g_confirm_render},
    {G_MENUSCREEN_ONRENDER, (void *)menu_render_hook, &g_menu_render},
    {G_OPTIONS_ONRENDER, (void *)options_render_hook, &g_options_render},
    {G_CHOICEBOX_ONRENDER, (void *)choice_render_hook, &g_choice_render},
    {G_LANGUAGE_ONRENDER, (void *)language_render_hook, &g_language_render},
    {G_STARMAP_ONRENDER, (void *)starmap_render_hook, &g_starmap_render},
    {G_STORE_ONRENDER, (void *)store_render_hook, &g_store_render},
    {G_TABBED_ONRENDER, (void *)tabbed_render_hook, &g_tabbed_render},
    {G_INPUTBOX_ONRENDER, (void *)input_render_hook, &g_input_render},
    {G_GAMEOVER_ONRENDER, (void *)gameover_render_hook, &g_gameover_render},
};

static void (*g_ship_render)(void *ship_manager, bool interior, bool doors);
static void ship_render_hook(void *sm, bool interior, bool doors) {
    add_ship(sm);
    g_ship_render(sm, interior, doors);
}

int targets_get(ux_target *out, int max) {
    pthread_mutex_lock(&g_lock);
    int n = g_npub < max ? g_npub : max;
    memcpy(out, g_pub, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&g_lock);
    return n;
}

void targets_set_frame_fn(void (*fn)(void *app)) { g_frame_fn = fn; }

int targets_install(mo_image *img) {
    g_matrix = mo_ptr(img, G_CSURFACE_MATRIX);
    g_capp_render = patch_hook(img, G_CAPP_ONRENDER, (void *)capp_render_hook, 1);
    g_button_render = patch_hook(img, G_BUTTON_ONRENDER, (void *)button_render_hook, 1);
    g_textbutton_render = patch_hook(img, G_TEXTBUTTON_ONRENDER, (void *)textbutton_render_hook, 1);
    g_ship_render = patch_hook(img, G_SHIPMANAGER_ONRENDER, (void *)ship_render_hook, 1);
    bool windows = g_ship_render != NULL;
    for (size_t i = 0; i < sizeof WINDOWS / sizeof *WINDOWS; i++) {
        *WINDOWS[i].orig = patch_hook(img, WINDOWS[i].addr, WINDOWS[i].hook, 1);
        windows &= *WINDOWS[i].orig != NULL;
    }
    if (!windows) LOGW("targets: some window hooks couldn't be placed");
    if (g_capp_render && g_button_render && g_textbutton_render) return 0;
    LOGW("targets: some hooks couldn't be placed; the gamepad cursor won't be drawn to buttons");
    return -1;
}
