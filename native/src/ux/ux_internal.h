// Shared between ux.c and pad.c (main thread unless noted).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// ux.c
uint8_t *ux_capp(void);               // CApp *, or NULL before the game runs
int ux_toggle_pause(void);            // the new state, -1 when not in a run
bool ux_paused(void);                 // in a run, and paused
void ux_target_view(double v[4]);     // where the view is going: x0, y0, x1, y1 in game px
void ux_zoom_at(double factor, double sx, double sy);  // keeping the game point under (sx, sy) in place
void ux_pan_by(double dx, double dy);  // surface px; only when zoomed in
void ux_synth_touch_game(int phase, double gx, double gy);  // the mouse's and the pad's finger

// pad.c
void pad_tick(void);
void pad_cursor_hide(void);           // a finger or the mouse took over
void pad_set_option(const char *key, float value);
void pad_frame(void *app);            // game's thread, start of each frame (rumble)
