// Binary patches / hooks applied to the loaded game image before it runs.
#pragma once
#include <stdint.h>

struct mo_image;
int patches_apply(struct mo_image *img);
// Redirect the function at link-time address `addr` to `hook`; returns a
// callable trampoline to the original (or NULL if not needed).
void *patch_hook(struct mo_image *img, uint64_t addr, void *hook, int want_original);
int patch_branch(struct mo_image *img, uint64_t addr, void *target);
