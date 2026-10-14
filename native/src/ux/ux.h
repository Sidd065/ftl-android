// Phone UX layer: screen layout, render resolution, touch handling, overlays.
#pragma once
#include <stdbool.h>

// layout[]: 0..3 safe insets (left, top, right, bottom) in surface pixels,
//           4 screen density (dpi), 5 render height (0 = auto),
//           6 magnifier (0 auto, 1 on, 2 off)
void ux_configure(int surface_w, int surface_h, int refresh_hz, const float *layout);
void ux_handle_touch(int phase, long id, float sx, float sy, double t);
void ux_handle_back(void);
void ux_quit(void);  // save and exit (after the player confirmed)
void ux_set_option(const char *key, float value);
void ux_handle_key(int android_keycode, bool down);
void ux_handle_mouse(int action, float sx, float sy, float wheel);  // see ux.c for actions
void ux_pad_connected(bool connected, const char *name);
void ux_pad_button(int android_keycode, bool down);
void ux_pad_axes(const float *axes);  // lx, ly, rx, ry, lt, rt, hat x, hat y
void ux_tick(void);                   // main thread, each run-loop pass
