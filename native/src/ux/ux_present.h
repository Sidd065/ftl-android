#pragma once
#include <stdbool.h>
void ux_present_view(double *x0, double *y0, double *x1, double *y1);
double ux_current_zoom(void);
bool ux_in_game(void);
bool ux_in_main_menu(void);
void ux_handle_key(int android_keycode, bool down);
void ux_handle_mouse(int action, float sx, float sy, float wheel);
void ux_reset_zoom(bool animate);
void ux_haptic(int kind);  // 1 magnifier shown, 2 pause toggled, 3 right click (if vibration is on)
