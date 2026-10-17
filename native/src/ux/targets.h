// What the gamepad cursor can be drawn to: the clickable things on screen, collected on
// the game's thread while it draws each frame. Rectangles in HUD units (the game's
// 1024x768 layout, x from the left of that 1024, so negative left of it on wide screens).
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct mo_image;

enum {
    TARGET_BUTTON,  // a button the game, Hyperspace or a mod drew this frame
    TARGET_CHOICE,  // an event choice
    TARGET_BEACON,  // a beacon on the map
    TARGET_TILE,    // a floor tile on either ship
    TARGET_DOOR,    // a door, while the door button is on
    TARGET_CREW,    // a crew member
    TARGET_BOX,     // a crew or system box on the HUD
};

typedef struct {
    float x, y, w, h;
    uint8_t kind;
} ux_target;

int targets_install(struct mo_image *img);
// Main thread: the targets of the last frame drawn. Returns how many (at most max).
int targets_get(ux_target *out, int max);
// Called on the game's thread at the start of each frame, with the CApp.
void targets_set_frame_fn(void (*fn)(void *app));
