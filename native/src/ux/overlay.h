// Port-side overlay drawn on top of the game image (render thread).
#pragma once
#include <stdbool.h>

enum { OVERLAY_ICON_PAUSE = 1, OVERLAY_ICON_PLAY };
void overlay_draw(int sw, int sh, unsigned game_fbo, int gw, int gh);
bool overlay_minimap_hit(float sx, float sy);   // main thread
void overlay_flash(int icon);                   // main thread
// Magnifier for the finger driving the game (main thread): phase 0 up, 1 down,
// 2 move, 3 down and show at once (explicit request, ignores the setting).
void overlay_loupe_touch(int phase, float sx, float sy);
void overlay_loupe_enable(bool on);
void overlay_minimap_enable(bool on);
void overlay_minimap_hide(bool hide);  // while zoomed in on a window
void overlay_cursor(bool visible, float sx, float sy);  // gamepad cursor (main thread)
// Outlines of what the gamepad cursor is drawn to, for checking them (main thread):
// x, y, w, h per rect in surface px and a colour each (0xRRGGBB); NULL or 0 to stop.
void overlay_debug_rects(const float *rects, const unsigned *colors, int n);
