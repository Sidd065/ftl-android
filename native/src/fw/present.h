// Final composite of the game's offscreen drawable onto the window surface.
#pragma once
#include <stdbool.h>
#include <GLES3/gl3.h>

typedef struct {
    // Where the game image lands on the surface, in surface pixels (top-left origin).
    int x, y, w, h;
} present_rect;

void present_frame(GLuint game_fbo, int game_w, int game_h);
void present_set_dest(present_rect r);
present_rect present_get_dest(void);
// surface pixel -> game drawable pixel; returns false if outside the game image
bool present_map_point(double sx, double sy, double *gx, double *gy);
// Called on the presenting thread just before swapping, for overlays.
typedef void (*present_overlay_fn)(int surface_w, int surface_h, unsigned game_fbo, int game_w, int game_h);
void present_set_overlay(present_overlay_fn fn);
// Source rectangle of the game image to show (game pixels); default = whole image.
typedef void (*present_view_fn)(double *x0, double *y0, double *x1, double *y1);
void present_set_view_fn(present_view_fn fn);
// Hook invoked after each present (e.g. screenshot capture in the harness).
typedef void (*present_post_fn)(int surface_w, int surface_h);
void present_set_post_hook(present_post_fn fn);
