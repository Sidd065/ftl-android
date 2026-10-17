// Platform abstraction between the iOS framework emulation and the host
// (Android via JNI/EGL/AAudio, or the Linux test harness).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// ---- lifecycle / events (main thread) ----
void plat_main_thread_started(void);
void plat_pump_events(void);   // deliver queued input/lifecycle events to fw_*
void plat_wake_main(void);     // wake the main thread from a wait

// ---- GL ----
void *plat_gl_create_context(int es_major, void *share);
bool plat_gl_make_current(void *ctx);   // binds the window surface when available
void plat_gl_present(void);             // swap the window surface (no-op without one)
bool plat_gl_has_surface(void);
void plat_gl_surface_size(int *w, int *h);
void *plat_gl_get_proc(const char *name);
unsigned plat_gl_window_fbo(void);       // framebuffer that represents the window (0 on real surfaces)

// ---- audio ----
typedef void (*plat_audio_cb)(void *ud, int16_t *out, int frames);
bool plat_audio_open(int rate, int channels, plat_audio_cb cb, void *ud);
void plat_audio_pause(bool paused);
int plat_audio_native_rate(void);

// ---- UI services ----
void plat_request_text_input(const char *prompt, const char *initial);
void plat_cancel_text_input(void);
void plat_show_alert(const char *title, const char *msg, const char **buttons, int nbuttons);
void plat_open_url(const char *url);
void plat_request_quit(void);  // ask the player to confirm quitting; yes -> ux_quit()
void plat_keep_screen_on(bool on);
void plat_haptic(int kind);  // short vibration: 1 magnifier shown, 2 pause toggled, 3 right click
void plat_pad_rumble(float strength, int ms);  // the game controller's motors, strength 0..1 (any thread)

// callbacks into the framework layer (implemented in fw/)
void fw_alert_result(int button);
