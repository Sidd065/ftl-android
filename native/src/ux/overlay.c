// Overlay drawn by the port on top of the game image: a zoom minimap (shown
// while zoomed in), a magnifier under a held finger, and a brief pause
// indicator for the two-finger tap.
// Runs on the game's GL thread inside present_frame; all touched GL state is
// saved and restored.
#include "overlay.h"

#include <GLES3/gl3.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "../fw/fw.h"
#include "../fw/present.h"
#include "port_log.h"
#include "ux_present.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---- tiny rounded-rect renderer ---------------------------------------------------
static GLuint g_prog, g_vao, g_vbo;
static GLint u_rect, u_screen, u_radius, u_border, u_fill, u_stroke, u_shape;

static const char *VS =
    "#version 300 es\n"
    "layout(location=0) in vec2 a_pos;\n"
    "uniform vec4 u_rect; uniform vec2 u_screen;\n"
    "out vec2 v_px;\n"
    "void main(){ vec2 p = u_rect.xy + a_pos*u_rect.zw; v_px = a_pos*u_rect.zw;\n"
    "  gl_Position = vec4(p.x/u_screen.x*2.0-1.0, 1.0-p.y/u_screen.y*2.0, 0.0, 1.0); }\n";
static const char *FS =
    "#version 300 es\n"
    "precision highp float;\n"  // u_rect is shared with the vertex shader: precisions must match
    "in vec2 v_px; uniform vec4 u_rect; uniform float u_radius; uniform float u_border;\n"
    "uniform vec4 u_fill; uniform vec4 u_stroke; uniform int u_shape; out vec4 o;\n"
    "float sdb(vec2 p, vec2 b, float r){ vec2 q = abs(p)-b+r; return length(max(q,0.0))+min(max(q.x,q.y),0.0)-r; }\n"
    "float sdt(vec2 p, vec2 s){\n"  // triangle (0,0) (w,h/2) (0,h), pointing right
    "  vec2 n1 = normalize(vec2(s.y*0.5, -s.x)), n2 = normalize(vec2(s.y*0.5, s.x));\n"
    "  return max(-p.x, max(dot(p, n1), dot(p-vec2(0.0, s.y), n2))); }\n"
    "void main(){ vec2 h = u_rect.zw*0.5;\n"
    "  float d = u_shape == 1 ? sdt(v_px, u_rect.zw) : sdb(v_px-h, h, u_radius);\n"
    "  float inside = clamp(0.5-d, 0.0, 1.0); float inner = clamp(0.5-(d+u_border), 0.0, 1.0);\n"
    "  vec4 c = mix(u_stroke, u_fill, inner); o = vec4(c.rgb, c.a*inside); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        LOGE("overlay shader: %s", log);
    }
    return s;
}
static bool g_gl_failed;
static bool init_gl(void) {
    if (g_gl_failed) return false;
    if (g_prog) return true;
    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs);
    glAttachShader(g_prog, fs);
    glLinkProgram(g_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetProgramInfoLog(g_prog, sizeof log, NULL, log);
        LOGE("overlay program link failed: %s", log);
        glDeleteProgram(g_prog);
        g_prog = 0;
        g_gl_failed = true;
        return false;
    }
    u_rect = glGetUniformLocation(g_prog, "u_rect");
    u_screen = glGetUniformLocation(g_prog, "u_screen");
    u_radius = glGetUniformLocation(g_prog, "u_radius");
    u_border = glGetUniformLocation(g_prog, "u_border");
    u_fill = glGetUniformLocation(g_prog, "u_fill");
    u_stroke = glGetUniformLocation(g_prog, "u_stroke");
    u_shape = glGetUniformLocation(g_prog, "u_shape");
    static const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    return true;
}

static void rrect(float x, float y, float w, float h, float r, float border, const float fill[4],
                  const float stroke[4]) {
    glUniform4f(u_rect, x, y, w, h);
    glUniform1f(u_radius, r);
    glUniform1f(u_border, border);
    glUniform4fv(u_fill, 1, fill);
    glUniform4fv(u_stroke, 1, stroke);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void triangle(float x, float y, float w, float h, const float fill[4]) {
    const float none[4] = {0, 0, 0, 0};
    glUniform1i(u_shape, 1);
    rrect(x, y, w, h, 0, 0, fill, none);
    glUniform1i(u_shape, 0);
}

// ---- state shared with the main thread ----------------------------------------------
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_mm_x, g_mm_y, g_mm_w, g_mm_h;  // minimap rect (surface px), 0 when hidden
static double g_flash_t;
static int g_flash_icon;
static bool g_minimap_on = true, g_minimap_hidden;
static bool g_cursor_on;
static float g_cursor_x, g_cursor_y;
#define MAX_DEBUG_RECTS 768
static float g_debug_rects[MAX_DEBUG_RECTS * 4];
static unsigned g_debug_colors[MAX_DEBUG_RECTS];
static int g_ndebug;
// magnifier
static bool g_loupe_on, g_lp_down, g_lp_moved, g_lp_force;
static float g_lp_x, g_lp_y, g_lp_x0, g_lp_y0;
static double g_lp_t0;
static const double LOUPE_DELAY = 0.22;  // s held before the magnifier shows (quick taps stay clean)
static const float LOUPE_MAG = 2.0f;     // magnification relative to the screen

bool overlay_minimap_hit(float sx, float sy) {
    pthread_mutex_lock(&g_lock);
    float pad = g_mm_h * 0.15f;
    bool hit = g_mm_w > 0 && sx >= g_mm_x - pad && sx <= g_mm_x + g_mm_w + pad && sy >= g_mm_y - pad &&
               sy <= g_mm_y + g_mm_h + pad;
    pthread_mutex_unlock(&g_lock);
    return hit;
}
void overlay_minimap_enable(bool on) {
    pthread_mutex_lock(&g_lock);
    g_minimap_on = on;
    pthread_mutex_unlock(&g_lock);
}
void overlay_minimap_hide(bool hide) {
    pthread_mutex_lock(&g_lock);
    g_minimap_hidden = hide;
    pthread_mutex_unlock(&g_lock);
}
void overlay_loupe_enable(bool on) {
    pthread_mutex_lock(&g_lock);
    g_loupe_on = on;
    pthread_mutex_unlock(&g_lock);
}
void overlay_cursor(bool visible, float sx, float sy) {
    pthread_mutex_lock(&g_lock);
    g_cursor_on = visible;
    g_cursor_x = sx;
    g_cursor_y = sy;
    pthread_mutex_unlock(&g_lock);
}
void overlay_debug_rects(const float *rects, const unsigned *colors, int n) {
    if (n > MAX_DEBUG_RECTS) n = MAX_DEBUG_RECTS;
    pthread_mutex_lock(&g_lock);
    g_ndebug = rects ? n : 0;
    if (g_ndebug) {
        memcpy(g_debug_rects, rects, (size_t)g_ndebug * 4 * sizeof *rects);
        memcpy(g_debug_colors, colors, (size_t)g_ndebug * sizeof *colors);
    }
    pthread_mutex_unlock(&g_lock);
}
void overlay_loupe_touch(int phase, float sx, float sy) {
    pthread_mutex_lock(&g_lock);
    if (phase == 1 || phase == 3) {
        g_lp_down = true;
        g_lp_moved = phase == 3;
        g_lp_force = phase == 3;
        g_lp_t0 = now_s();
        g_lp_x0 = sx;
        g_lp_y0 = sy;
    } else if (phase == 0) {
        g_lp_down = false;
        g_lp_force = false;
    }
    if (phase == 2 && hypotf(sx - g_lp_x0, sy - g_lp_y0) > present_get_dest().h * 0.03f) g_lp_moved = true;
    g_lp_x = sx;
    g_lp_y = sy;
    pthread_mutex_unlock(&g_lock);
}
void overlay_flash(int icon) {
    pthread_mutex_lock(&g_lock);
    g_flash_icon = icon;
    g_flash_t = now_s();
    pthread_mutex_unlock(&g_lock);
}

// ---- drawing ------------------------------------------------------------------------
void overlay_draw(int sw, int sh, unsigned game_fbo, int gw, int gh) {
    double zoom = ux_current_zoom();
    double ft, lt;
    int ficon;
    float lx, ly, cx_, cy_;
    bool show_loupe, show_cursor, forced;
    static bool loupe_was_shown;
    pthread_mutex_lock(&g_lock);
    double now = now_s();
    ft = now - g_flash_t;
    ficon = g_flash_icon;
    lt = now - g_lp_t0;
    lx = g_lp_x;
    ly = g_lp_y;
    show_loupe = (g_loupe_on || g_lp_force) && g_lp_down && (lt >= LOUPE_DELAY || g_lp_moved) &&
                 zoom < LOUPE_MAG * 0.9;
    if (g_lp_moved && lt < LOUPE_DELAY) lt = LOUPE_DELAY;
    forced = g_lp_force;
    show_cursor = g_cursor_on;
    cx_ = g_cursor_x;
    cy_ = g_cursor_y;
    static float debug_rects[MAX_DEBUG_RECTS * 4];
    static unsigned debug_colors[MAX_DEBUG_RECTS];
    int ndebug = g_ndebug;
    memcpy(debug_rects, g_debug_rects, (size_t)ndebug * 4 * sizeof *debug_rects);
    memcpy(debug_colors, g_debug_colors, (size_t)ndebug * sizeof *debug_colors);
    pthread_mutex_unlock(&g_lock);
    if (show_loupe && !loupe_was_shown && !forced) ux_haptic(1);  // held long enough: the magnifier is up
    loupe_was_shown = show_loupe;
    bool show_mm = zoom > 1.01 && g_minimap_on && !g_minimap_hidden;
    bool show_flash = ficon && ft < 0.6;
    if (!show_mm && !show_flash && !show_loupe && !show_cursor && !ndebug) {
        pthread_mutex_lock(&g_lock);
        g_mm_w = 0;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    // save state (before init_gl, which binds our own VAO/buffer)
    GLint prog, vao, abuf, bsrc_rgb, bdst_rgb, bsrc_a, bdst_a, beq_rgb, beq_a;
    GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE),
              stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrc_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &bdst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &beq_a);
    if (!init_gl()) goto restore;

    present_rect d = present_get_dest();
    float unit = (float)d.h / 100.0f;  // 1% of the game area's height

    if (show_mm) {
        // Minimap: whole frame scaled into a panel in the top-left, with the view outlined.
        float mh = unit * 22, mw = mh * (float)gw / (float)gh;
        float mx = d.x + unit * 2.5f, my = d.y + unit * 2.5f;
        // panel background
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(g_prog);
        glBindVertexArray(g_vao);
        glUniform2f(u_screen, (float)sw, (float)sh);
        const float panel[4] = {0.02f, 0.05f, 0.08f, 0.85f}, edge[4] = {0.55f, 0.85f, 0.95f, 0.9f};
        float pad = unit * 0.8f;
        rrect(mx - pad, my - pad, mw + 2 * pad, mh + 2 * pad, unit * 1.2f, unit * 0.25f, panel, edge);
        // scaled game image
        GLint rfbo;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, game_fbo);
        glBlitFramebuffer(0, 0, gw, gh, (GLint)mx, sh - (GLint)(my + mh), (GLint)(mx + mw), sh - (GLint)my,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfbo);
        // view rectangle
        double vx0, vy0, vx1, vy1;
        ux_present_view(&vx0, &vy0, &vx1, &vy1);
        const float clear[4] = {0, 0, 0, 0}, view_edge[4] = {1.0f, 0.85f, 0.3f, 1.0f};
        float rx = mx + (float)(vx0 / gw) * mw, ry = my + (float)(vy0 / gh) * mh;
        float rw = (float)((vx1 - vx0) / gw) * mw, rh = (float)((vy1 - vy0) / gh) * mh;
        rrect(rx, ry, rw, rh, unit * 0.3f, unit * 0.35f, clear, view_edge);
        pthread_mutex_lock(&g_lock);
        g_mm_x = mx;
        g_mm_y = my;
        g_mm_w = mw;
        g_mm_h = mh;
        pthread_mutex_unlock(&g_lock);
    } else {
        pthread_mutex_lock(&g_lock);
        g_mm_w = 0;
        pthread_mutex_unlock(&g_lock);
    }

    if (show_loupe) {
        // Magnified view of the area under the finger, placed where the hand
        // doesn't cover it (above, else beside), with the touch point marked.
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(g_prog);
        glBindVertexArray(g_vao);
        glUniform2f(u_screen, (float)sw, (float)sh);
        float a = (float)fmin(1.0, (lt - LOUPE_DELAY) / 0.08 + 0.35);
        float L = unit * 32, pad = unit * 0.7f, gap = unit * 10;
        float cx = lx, cy = ly - (L / 2 + gap);
        if (cy - L / 2 < d.y + unit) {  // no room above: beside the finger, away from the nearer edge
            cy = ly;
            cx = lx > d.x + d.w / 2.0f ? lx - (L / 2 + gap) : lx + (L / 2 + gap);
        }
        cx = fminf(fmaxf(cx, d.x + L / 2 + unit), d.x + d.w - L / 2 - unit);
        cy = fminf(fmaxf(cy, d.y + L / 2 + unit), d.y + d.h - L / 2 - unit);
        float x0 = cx - L / 2, y0 = cy - L / 2;
        const float panel[4] = {0.02f, 0.05f, 0.08f, 0.9f * a}, edge[4] = {0.55f, 0.85f, 0.95f, 0.95f * a};
        rrect(x0 - pad, y0 - pad, L + 2 * pad, L + 2 * pad, unit * 1.4f, unit * 0.3f, panel, edge);
        // source rect in game pixels around the touch point
        double vx0, vy0, vx1, vy1;
        ux_present_view(&vx0, &vy0, &vx1, &vy1);
        double gpp = (vx1 - vx0) / d.w;  // game px per surface px
        double gx = vx0 + (lx - d.x) * gpp, gy = vy0 + (ly - d.y) * gpp;
        double half = L / LOUPE_MAG * gpp / 2;
        double sx0 = fmin(fmax(gx - half, 0), gw - 2 * half), sy0 = fmin(fmax(gy - half, 0), gh - 2 * half);
        GLint rfbo;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, game_fbo);
        glBlitFramebuffer((GLint)sx0, gh - (GLint)(sy0 + 2 * half), (GLint)(sx0 + 2 * half), gh - (GLint)sy0,
                          (GLint)x0, sh - (GLint)(y0 + L), (GLint)(x0 + L), sh - (GLint)y0, GL_COLOR_BUFFER_BIT,
                          GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfbo);
        // touch point marker (offset when the source rect was clamped at an edge)
        float mx = x0 + (float)((gx - sx0) / (2 * half)) * L, my = y0 + (float)((gy - sy0) / (2 * half)) * L;
        float r = unit * 1.6f;
        const float clear[4] = {0, 0, 0, 0}, ring[4] = {1.0f, 0.85f, 0.3f, 0.95f * a},
                    dark[4] = {0, 0, 0, 0.6f * a};
        rrect(mx - r - unit * 0.25f, my - r - unit * 0.25f, 2 * r + unit * 0.5f, 2 * r + unit * 0.5f,
              r + unit * 0.25f, unit * 0.2f, clear, dark);
        rrect(mx - r, my - r, 2 * r, 2 * r, r, unit * 0.3f, clear, ring);
    }

    if (ndebug) {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(g_prog);
        glBindVertexArray(g_vao);
        glUniform2f(u_screen, (float)sw, (float)sh);
        const float clear[4] = {0, 0, 0, 0};
        for (int i = 0; i < ndebug; i++) {
            unsigned c = debug_colors[i];
            const float edge[4] = {(c >> 16 & 255) / 255.0f, (c >> 8 & 255) / 255.0f, (c & 255) / 255.0f, 0.9f};
            rrect(debug_rects[i * 4], debug_rects[i * 4 + 1], debug_rects[i * 4 + 2], debug_rects[i * 4 + 3], 0,
                  unit * 0.18f, clear, edge);
        }
    }

    if (show_cursor) {
        // gamepad cursor: a ring with a dot at the hotspot, outlined for contrast
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(g_prog);
        glBindVertexArray(g_vao);
        glUniform2f(u_screen, (float)sw, (float)sh);
        const float clear[4] = {0, 0, 0, 0}, dark[4] = {0, 0, 0, 0.7f}, ring[4] = {1.0f, 0.85f, 0.3f, 1.0f};
        float r = unit * 1.9f, o = unit * 0.3f, dot = unit * 0.45f;
        rrect(cx_ - r - o, cy_ - r - o, 2 * (r + o), 2 * (r + o), r + o, unit * 0.25f, clear, dark);
        rrect(cx_ - r, cy_ - r, 2 * r, 2 * r, r, unit * 0.35f, clear, ring);
        rrect(cx_ - dot - o, cy_ - dot - o, 2 * (dot + o), 2 * (dot + o), dot + o, 0, dark, dark);
        rrect(cx_ - dot, cy_ - dot, 2 * dot, 2 * dot, dot, 0, ring, ring);
    }

    if (show_flash) {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(g_prog);
        glBindVertexArray(g_vao);
        glUniform2f(u_screen, (float)sw, (float)sh);
        float a = (float)(1.0 - ft / 0.6);
        float s = unit * 16, cx = d.x + d.w / 2.0f, cy = d.y + d.h / 2.0f;
        const float bg[4] = {0.02f, 0.05f, 0.08f, 0.55f * a}, none[4] = {0, 0, 0, 0};
        const float bar[4] = {0.95f, 0.95f, 0.95f, 0.9f * a};
        rrect(cx - s / 2, cy - s / 2, s, s, s / 2, 0, bg, none);
        if (ficon == OVERLAY_ICON_PLAY) {
            float tw = s * 0.36f, th = s * 0.42f;
            triangle(cx - tw * 0.38f, cy - th / 2, tw, th, bar);
        } else {
            float bw = s * 0.14f, bh = s * 0.46f;
            rrect(cx - bw * 1.6f, cy - bh / 2, bw, bh, bw * 0.3f, 0, bar, none);
            rrect(cx + bw * 0.6f, cy - bh / 2, bw, bh, bw * 0.3f, 0, bar, none);
        }
    }

restore:
    glUseProgram((GLuint)prog);
    glBindVertexArray((GLuint)vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
    glBlendFuncSeparate((GLenum)bsrc_rgb, (GLenum)bdst_rgb, (GLenum)bsrc_a, (GLenum)bdst_a);
    glBlendEquationSeparate((GLenum)beq_rgb, (GLenum)beq_a);
    if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
}
