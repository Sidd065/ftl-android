// Fixes for bugs in the game itself (see fixes.c).
#pragma once

struct mo_image;
int fixes_install(struct mo_image *img);  // before the image is protected; 0 on success
