#pragma once
#include <stdbool.h>
#include <stddef.h>

struct mo_image;

// Load libHyperspace.so and let it hook the game. Returns 0 on success; otherwise
// writes a message for the player to err and the game must not be started.
int hyperspace_start(const char *lib, const char *log_dir, const char *data_dir, struct mo_image *img, char *err,
                     size_t errsize);

// Right clicks for mods' scripts (see hyperspace_host.h). Off when Hyperspace isn't
// running or no script listens for the right button. x and y in game pixels, as touches.
bool hyperspace_wants_right_click(void);
void hyperspace_right_button(int action, double x, double y);
