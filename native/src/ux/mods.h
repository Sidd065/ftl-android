// Mods: an overlay of modded files in front of the game's ftl.dat (see mods.c).
#pragma once

struct mo_image;
// index_path: the overlay index built by the app (NULL or "": no mods).
// Call before the image is protected. Returns 0 if installed or not wanted.
int mods_install(struct mo_image *img, const char *index_path);
