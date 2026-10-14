#pragma once
#include <stddef.h>
#include <stdint.h>
// Returns malloc'd RGBA8 pixels (w*h*4) or NULL if not a PVRTC format / too little data.
uint8_t *pvrtc_decode(const void *data, size_t size, int w, int h, unsigned glformat);
int pvrtc_format_info(unsigned glformat, int *bpp2, int *has_alpha);
