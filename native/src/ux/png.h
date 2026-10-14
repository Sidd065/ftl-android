// PNG support for the iPad build, which was compiled without it (see png.c).
#pragma once

struct mo_image;
int png_install(struct mo_image *img);  // before the image is protected; 0 on success
