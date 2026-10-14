// Mods: ftl.dat is patched by ftlman; this makes mods' images show (see mods.c).
#pragma once

struct mo_image;
// index_path: the port's overlay index written by the app (NULL or "": no mods).
// Call before the image is protected. Returns 0 if installed or not wanted.
int mods_install(struct mo_image *img, const char *index_path);
