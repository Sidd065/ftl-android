#include "present.h"

#include <math.h>
#include <pthread.h>
#include <time.h>

#include "../platform/platform.h"
#include "fw.h"
#include "port_log.h"

static present_rect g_dest;
static bool g_dest_set;
static present_overlay_fn g_overlay;
static present_post_fn g_post;
static present_view_fn g_view;
static pthread_mutex_t g_dest_lock = PTHREAD_MUTEX_INITIALIZER;

void present_set_dest(present_rect r) {
    pthread_mutex_lock(&g_dest_lock);
    g_dest = r;
    g_dest_set = true;
    pthread_mutex_unlock(&g_dest_lock);
}
present_rect present_get_dest(void) {
    pthread_mutex_lock(&g_dest_lock);
    present_rect r = g_dest;
    if (!g_dest_set) r = (present_rect){0, 0, g_display.surface_w, g_display.surface_h};
    pthread_mutex_unlock(&g_dest_lock);
    return r;
}
void present_set_overlay(present_overlay_fn fn) { g_overlay = fn; }
void present_set_post_hook(present_post_fn fn) { g_post = fn; }
void present_set_view_fn(present_view_fn fn) { g_view = fn; }

bool present_map_point(double sx, double sy, double *gx, double *gy) {
    present_rect r = present_get_dest();
    if (r.w <= 0 || r.h <= 0) return false;
    double u = (sx - r.x) / r.w, v = (sy - r.y) / r.h;
    *gx = u * g_display.render_w;
    *gy = v * g_display.render_h;
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

// Frame pacing stats, logged every 30 s: average rate and the longest gap.
static void frame_stats(void) {
    static double t0, last, worst;
    static int frames;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (t0 == 0) t0 = last = now;
    if (now - last > worst) worst = now - last;
    last = now;
    frames++;
    if (now - t0 >= 30) {
        long free_mb, total_mb, game_mb;
        port_memory(&free_mb, &total_mb, &game_mb);
        LOGI("present: %.1f fps over %.0f s, longest frame %.0f ms; %ld MB free, the game holds %ld MB",
             frames / (now - t0), now - t0, worst * 1000, free_mb, game_mb);
        t0 = now;
        frames = 0;
        worst = 0;
    }
}

void present_frame(GLuint game_fbo, int gw, int gh) {
    if (!plat_gl_has_surface()) return;
    frame_stats();
    int sw, sh;
    plat_gl_surface_size(&sw, &sh);
    if (sw <= 0 || sh <= 0) return;

    GLint draw_fbo, read_fbo, viewport[4];
    GLboolean scissor, mask[4];
    GLfloat clear[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);

    present_rect r = present_get_dest();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, plat_gl_window_fbo());
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, sw, sh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, game_fbo);
    // Source: the visible part of the game image (zoom), in GL's bottom-left space.
    double vx0 = 0, vy0 = 0, vx1 = gw, vy1 = gh;
    if (g_view) g_view(&vx0, &vy0, &vx1, &vy1);
    int sx0 = (int)lround(vx0), sx1 = (int)lround(vx1);
    int sy0 = gh - (int)lround(vy1), sy1 = gh - (int)lround(vy0);
    // GL's origin is bottom-left; our rect is top-left based.
    int dx0 = r.x, dx1 = r.x + r.w;
    int dy0 = sh - (r.y + r.h), dy1 = sh - r.y;
    bool exact = (sx1 - sx0) == (dx1 - dx0) && (sy1 - sy0) == (dy1 - dy0);
    // A portrait surface (lock screen, rotation in progress) gets a black frame
    // rather than the landscape game squeezed into it.
    bool portrait_glitch = sw < sh && r.w > r.h;
    if (!portrait_glitch) {
        glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT,
                          exact ? GL_NEAREST : GL_LINEAR);
        if (g_overlay) g_overlay(sw, sh, game_fbo, gw, gh);
    }
    if (g_post) g_post(sw, sh);
    plat_gl_present();

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    GLenum err = glGetError();
    static int reported;
    if (err && reported++ < 5) LOGW("present: GL error %#x", err);
}
