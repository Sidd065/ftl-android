// QuartzCore (CALayer, CAEAGLLayer, CADisplayLink) and OpenGLES.framework
// (EAGLContext) on top of EGL/GLES3. The game's "drawable" renderbuffer is a
// real offscreen renderbuffer; presentRenderbuffer: composites it onto the
// window surface (scaling/letterboxing as configured) and swaps.
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <GLES3/gl3.h>

#include "../platform/platform.h"
#include "fw.h"
#include "port_log.h"
#include "present.h"
#include "pvrtc.h"

Class cls_CALayer, cls_CAEAGLLayer, cls_CADisplayLink, cls_EAGLContext, cls_EAGLSharegroup;

#define M(cls, sel, fn) rt_method(cls, sel, (void *)(fn))
#define CM(cls, sel, fn) rt_cmethod(cls, sel, (void *)(fn))

// ---------------------------------------------------------------------------
// CALayer / CAEAGLLayer
typedef struct { Class isa; CGRect frame; double scale; id props; bool opaque; } LayerObj;
static id layer_init(id self, SEL _c) { (void)_c; ((LayerObj *)self)->scale = 1.0; return self; }
static void layer_setFrame(id self, SEL _c, CGRect f) { (void)_c; ((LayerObj *)self)->frame = f; }
static CGRect layer_frame(id self, SEL _c) { (void)_c; return ((LayerObj *)self)->frame; }
static CGRect layer_bounds(id self, SEL _c) {
    (void)_c;
    LayerObj *l = (LayerObj *)self;
    return CGRectMake(0, 0, l->frame.size.width, l->frame.size.height);
}
static void layer_setContentsScale(id self, SEL _c, double s) { (void)_c; ((LayerObj *)self)->scale = s; }
static double layer_contentsScale(id self, SEL _c) { (void)_c; return ((LayerObj *)self)->scale; }
static void layer_setOpaque(id self, SEL _c, BOOL o) { (void)_c; ((LayerObj *)self)->opaque = o; }
static void layer_setDrawableProperties(id self, SEL _c, id p) {
    (void)_c;
    LayerObj *l = (LayerObj *)self;
    if (p) MSG(id)(p, SEL_("retain"));
    if (l->props) MSG(void)(l->props, SEL_("release"));
    l->props = p;
}
static id layer_drawableProperties(id self, SEL _c) { (void)_c; return ((LayerObj *)self)->props; }
static void layer_noop1(id self, SEL _c, id a) { (void)self; (void)_c; (void)a; }

// ---------------------------------------------------------------------------
// CADisplayLink
typedef struct {
    Class isa;
    id target;
    SEL sel;
    bool paused, invalid, added;
    NSInteger preferred_fps;
    NSInteger frame_interval;
    double timestamp, duration, target_ts;
    unsigned long counter;
} LinkObj;
extern void uikit_register_display_link(id link);
extern void uikit_unregister_display_link(id link);

static id link_withTarget(Class self, SEL _c, id target, SEL sel) {
    (void)_c;
    LinkObj *l = (LinkObj *)class_createInstance(self, 0);
    l->target = MSG(id)(target, SEL_("retain"));
    l->sel = sel;
    l->frame_interval = 1;
    return rt_autorelease((id)l);
}
static void link_addToRunLoop(id self, SEL _c, id rl, id mode) {
    (void)_c; (void)rl; (void)mode;
    LinkObj *l = (LinkObj *)self;
    if (l->added) return;
    l->added = true;
    MSG(id)(self, SEL_("retain"));
    uikit_register_display_link(self);
    LOGI("CADisplayLink added (target %s, sel %s)", class_getName(object_getClass(l->target)), l->sel);
}
static void link_invalidate(id self, SEL _c) {
    (void)_c;
    LinkObj *l = (LinkObj *)self;
    if (l->invalid) return;
    l->invalid = true;
    if (l->added) {
        uikit_unregister_display_link(self);
        MSG(void)(self, SEL_("release"));
    }
}
static void link_setPaused(id self, SEL _c, BOOL p) { (void)_c; ((LinkObj *)self)->paused = p; }
static BOOL link_isPaused(id self, SEL _c) { (void)_c; return ((LinkObj *)self)->paused; }
static void link_setPreferredFPS(id self, SEL _c, NSInteger fps) { (void)_c; ((LinkObj *)self)->preferred_fps = fps; }
static void link_setFrameInterval(id self, SEL _c, NSInteger fi) { (void)_c; ((LinkObj *)self)->frame_interval = fi > 0 ? fi : 1; }
static double link_timestamp(id self, SEL _c) { (void)_c; return ((LinkObj *)self)->timestamp; }
static double link_duration(id self, SEL _c) { (void)_c; return ((LinkObj *)self)->duration; }
static double link_targetTimestamp(id self, SEL _c) { (void)_c; return ((LinkObj *)self)->target_ts; }

bool quartz_link_active(id link) {
    LinkObj *l = (LinkObj *)link;
    return !l->invalid && !l->paused;
}
void quartz_fire_display_link(id link, double ts, double duration) {
    LinkObj *l = (LinkObj *)link;
    l->counter++;
    NSInteger interval = l->frame_interval;
    if (l->preferred_fps > 0 && g_display.refresh_hz > l->preferred_fps)
        interval = (g_display.refresh_hz + l->preferred_fps / 2) / l->preferred_fps;
    if (interval > 1 && (l->counter % (unsigned long)interval) != 0) return;
    l->timestamp = ts;
    l->duration = duration;
    l->target_ts = ts + duration;
    MSG(void, id)(l->target, l->sel, link);
}

// ---------------------------------------------------------------------------
// EAGLContext
typedef struct { Class isa; NSUInteger api; void *plat; id sharegroup; } CtxObj;
static pthread_key_t g_cur_ctx_key;
static id g_sharegroup;

typedef struct {
    GLuint fbo;        // game FBO that has the drawable attached
    GLuint rb;         // drawable color renderbuffer
    int w, h;
    bool valid;
} drawable_state;
static drawable_state g_drawable;

static id sg_get(void) {
    if (!g_sharegroup) g_sharegroup = class_createInstance(cls_EAGLSharegroup, 0);
    return g_sharegroup;
}
static id ctx_initWithAPI_share(id self, SEL _c, NSUInteger api, id sg) {
    (void)_c; (void)sg;
    CtxObj *c = (CtxObj *)self;
    void *share = NULL;
    c->plat = plat_gl_create_context(api >= 3 ? 3 : 2, share);
    if (!c->plat) {
        LOGW("EAGLContext initWithAPI:%lu failed", api);
        MSG(void)(self, SEL_("release"));
        return nil;
    }
    c->api = api;
    c->sharegroup = sg_get();
    LOGI("EAGLContext initWithAPI:%lu -> %p", api, c->plat);
    return self;
}
static id ctx_initWithAPI(id self, SEL _c, NSUInteger api) { return ctx_initWithAPI_share(self, _c, api, nil); }
static NSUInteger ctx_API(id self, SEL _c) { (void)_c; return ((CtxObj *)self)->api; }
static id ctx_sharegroup(id self, SEL _c) { (void)_c; return ((CtxObj *)self)->sharegroup; }
static BOOL ctx_setCurrentContext(Class self, SEL _c, id ctx) {
    (void)self; (void)_c;
    id old = pthread_getspecific(g_cur_ctx_key);
    if (old == ctx) return YES;
    bool ok = plat_gl_make_current(ctx ? ((CtxObj *)ctx)->plat : NULL);
    if (ctx) MSG(id)(ctx, SEL_("retain"));
    if (old) MSG(void)(old, SEL_("release"));
    pthread_setspecific(g_cur_ctx_key, ctx);
    return ok;
}
static id ctx_currentContext(Class self, SEL _c) { (void)self; (void)_c; return pthread_getspecific(g_cur_ctx_key); }

static BOOL ctx_renderbufferStorage(id self, SEL _c, NSUInteger target, id drawable) {
    (void)self; (void)_c; (void)drawable;
    GLint rb = 0, fbo = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    int w = g_display.render_w, h = g_display.render_h;
    glRenderbufferStorage((GLenum)target, GL_RGBA8, w, h);
    g_drawable = (drawable_state){(GLuint)fbo, (GLuint)rb, w, h, true};
    GLenum err = glGetError();
    LOGI("renderbufferStorage:fromDrawable: rb=%d fbo=%d %dx%d err=%#x", rb, fbo, w, h, err);
    return YES;
}

static BOOL ctx_presentRenderbuffer(id self, SEL _c, NSUInteger target) {
    (void)self; (void)_c; (void)target;
    if (!g_drawable.valid) return NO;
    present_frame(g_drawable.fbo, g_drawable.w, g_drawable.h);
    return YES;
}

// ---------------------------------------------------------------------------
// GL entry points handed to the game via dlsym()
static void GL_APIENTRY wrap_glDiscardFramebufferEXT(GLenum target, GLsizei n, const GLenum *att) {
    glInvalidateFramebuffer(target, n, att);
}
static const GLubyte *GL_APIENTRY wrap_glGetString(GLenum name) {
    const GLubyte *s = glGetString(name);
    return s;
}

// iOS accepts GL_BGRA (APPLE_texture_format_BGRA8888) for textures; most
// Android drivers don't (at least not for glTexStorage2D). Store such
// textures as RGBA and swizzle client-side uploads/readbacks.
#define GL_BGRA_EXT_ 0x80E1
#define GL_BGRA8_EXT_ 0x93A1
static int is_bgra(GLenum f) { return f == GL_BGRA_EXT_ || f == GL_BGRA8_EXT_; }
static void *swizzle_copy(const void *src, GLsizei w, GLsizei h) {
    size_t n = (size_t)w * (size_t)h;
    uint32_t *out = malloc(n * 4);
    const uint32_t *in = src;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = in[i];
        out[i] = (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu);
    }
    return out;
}
static int unpack_pbo_bound(void) {
    GLint b = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &b);
    return b != 0;
}
static int g_gltrace = -1;
static void trace_tex(const char *fn, GLint ifmt, GLenum fmt, GLenum type, GLsizei w, GLsizei h) {
    if (g_gltrace < 0) g_gltrace = getenv("FTL_GLTRACE") != NULL;
    if (!g_gltrace) return;
    GLint tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    LOGI("gltrace %s tex=%d ifmt=%#x fmt=%#x type=%#x %dx%d", fn, tex, ifmt, fmt, type, w, h);
}

// A texture larger than the GPU takes (GL_MAX_TEXTURE_SIZE, 8192 on many Mali and PowerVR
// GPUs) can't be stored and draws as a black box. Mods have animation sheets with all their
// frames in one row, up to about 13000 pixels wide in Multiverse. Such a texture is stored
// at a fraction of its size along the long side, and its uploads are scaled down to match.
// The game's texture coordinates are relative, so it draws the same, a little less sharp.
typedef struct { GLuint tex; int sx, sy; GLsizei w, h; } Shrunk;
static Shrunk *g_shrunk;
static size_t g_nshrunk;
static GLint g_max_tex;

static int shrink_by(GLsizei n) {
    if (!g_max_tex) {
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_max_tex);
        if (g_max_tex <= 0) g_max_tex = 2048;
    }
    int f = 1;
    while ((n + f - 1) / f > g_max_tex) f++;
    return f;
}
static Shrunk *shrunk_find(void) {
    GLint tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    for (size_t i = 0; i < g_nshrunk; i++)
        if (g_shrunk[i].tex == (GLuint)tex) return &g_shrunk[i];
    return NULL;
}
static void shrunk_forget(void) {
    Shrunk *s = g_nshrunk ? shrunk_find() : NULL;
    if (s) *s = g_shrunk[--g_nshrunk];
}
// The bound texture gets its size (level 0): note whether it has to be stored smaller
static Shrunk *shrunk_size(GLsizei w, GLsizei h) {
    int sx = shrink_by(w), sy = shrink_by(h);
    if (sx == 1 && sy == 1) {
        shrunk_forget();
        return NULL;
    }
    Shrunk *s = shrunk_find();
    if (!s) {
        Shrunk *more = realloc(g_shrunk, (g_nshrunk + 1) * sizeof *g_shrunk);
        if (!more) return NULL;
        g_shrunk = more;
        s = &g_shrunk[g_nshrunk++];
    }
    GLint tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    *s = (Shrunk){(GLuint)tex, sx, sy, (w + sx - 1) / sx, (h + sy - 1) / sy};
    LOGW("texture %dx%d is over the GPU's limit of %d: stored at %dx%d", w, h, g_max_tex, s->w, s->h);
    return s;
}
static int channels(GLenum fmt) {
    switch (fmt) {
    case GL_RGBA: return 4;
    case GL_RGB: return 3;
    case GL_LUMINANCE_ALPHA: case GL_RG: return 2;
    case GL_LUMINANCE: case GL_ALPHA: case GL_RED: return 1;
    }
    return 0;
}
// Upload part of a shrunk texture: each sx x sy block of pixels becomes one, its colour
// weighted by alpha so edges don't darken
static void shrunk_upload(const Shrunk *s, GLenum t, GLint lvl, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt,
                          GLenum type, const void *px) {
    int ch = channels(fmt);
    if (!px || type != GL_UNSIGNED_BYTE || !ch || unpack_pbo_bound()) {
        LOGW("texture upload %dx%d (fmt %#x type %#x) can't be scaled down; left out", w, h, fmt, type);
        return;
    }
    GLint align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    size_t stride = ((size_t)w * ch + align - 1) / align * align;
    GLsizei nw = (w + s->sx - 1) / s->sx, nh = (h + s->sy - 1) / s->sy;
    uint8_t *out = malloc((size_t)nw * nh * ch);
    if (!out) return;
    const uint8_t *src = px;
    for (GLsizei oy = 0; oy < nh; oy++) {
        for (GLsizei ox = 0; ox < nw; ox++) {
            uint32_t sum[4] = {0}, alpha = 0, n = 0;
            for (int j = 0; j < s->sy && oy * s->sy + j < h; j++) {
                const uint8_t *p = src + (size_t)(oy * s->sy + j) * stride + (size_t)ox * s->sx * ch;
                for (int i = 0; i < s->sx && ox * s->sx + i < w; i++, p += ch, n++) {
                    if (ch == 4) {
                        for (int c = 0; c < 3; c++) sum[c] += p[c] * p[3];
                        alpha += p[3];
                    } else {
                        for (int c = 0; c < ch; c++) sum[c] += p[c];
                    }
                }
            }
            uint8_t *o = out + ((size_t)oy * nw + ox) * ch;
            if (ch == 4) {
                for (int c = 0; c < 3; c++) o[c] = alpha ? (uint8_t)((sum[c] + alpha / 2) / alpha) : 0;
                o[3] = (uint8_t)((alpha + n / 2) / n);
            } else {
                for (int c = 0; c < ch; c++) o[c] = (uint8_t)((sum[c] + n / 2) / n);
            }
        }
    }
    // Keep to the level's stored size when a piece doesn't start on a block boundary
    GLsizei lw = s->w >> lvl > 0 ? s->w >> lvl : 1, lh = s->h >> lvl > 0 ? s->h >> lvl : 1;
    GLint dx = x / s->sx, dy = y / s->sy;
    GLsizei cw = dx + nw > lw ? lw - dx : nw, chh = dy + nh > lh ? lh - dy : nh;
    if (cw > 0 && chh > 0) {
        GLint row = 0;
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, nw);
        glTexSubImage2D(t, lvl, dx, dy, cw, chh, fmt, type, out);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row);
        glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    }
    free(out);
}

static void GL_APIENTRY wrap_glTexImage2D(GLenum t, GLint lvl, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                                          GLenum fmt, GLenum type, const void *px) {
    trace_tex("TexImage2D", ifmt, fmt, type, w, h);
    void *tmp = NULL;
    if (is_bgra(fmt) || is_bgra((GLenum)ifmt)) {
        tmp = (px && type == GL_UNSIGNED_BYTE && !unpack_pbo_bound()) ? swizzle_copy(px, w, h) : NULL;
        if (tmp) px = tmp;
        ifmt = GL_RGBA;
        fmt = GL_RGBA;
    }
    const Shrunk *s = t != GL_TEXTURE_2D ? NULL : lvl == 0 ? shrunk_size(w, h) : shrunk_find();
    if (s) {
        GLsizei sw = (w + s->sx - 1) / s->sx, sh = (h + s->sy - 1) / s->sy;
        glTexImage2D(t, lvl, ifmt, sw, sh, border, fmt, type, NULL);
        if (px) shrunk_upload(s, t, lvl, 0, 0, w, h, fmt, type, px);
    } else {
        glTexImage2D(t, lvl, ifmt, w, h, border, fmt, type, px);
    }
    free(tmp);
}
static void GL_APIENTRY wrap_glTexSubImage2D(GLenum t, GLint lvl, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt,
                                             GLenum type, const void *px) {
    trace_tex("TexSubImage2D", 0, fmt, type, w, h);
    void *tmp = NULL;
    if (is_bgra(fmt)) {
        tmp = (px && type == GL_UNSIGNED_BYTE && !unpack_pbo_bound()) ? swizzle_copy(px, w, h) : NULL;
        if (tmp) px = tmp;
        fmt = GL_RGBA;
    }
    const Shrunk *s = t == GL_TEXTURE_2D && g_nshrunk ? shrunk_find() : NULL;
    if (s) shrunk_upload(s, t, lvl, x, y, w, h, fmt, type, px);
    else glTexSubImage2D(t, lvl, x, y, w, h, fmt, type, px);
    free(tmp);
}
static void GL_APIENTRY wrap_glRenderbufferStorage(GLenum t, GLenum ifmt, GLsizei w, GLsizei h) {
    glRenderbufferStorage(t, is_bgra(ifmt) ? GL_RGBA8 : ifmt, w, h);
}
static void GL_APIENTRY wrap_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *px) {
    if (is_bgra(fmt) && type == GL_UNSIGNED_BYTE) {
        glReadPixels(x, y, w, h, GL_RGBA, type, px);
        uint32_t *p = px;
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
            p[i] = (p[i] & 0xff00ff00u) | ((p[i] & 0xffu) << 16) | ((p[i] >> 16) & 0xffu);
        return;
    }
    glReadPixels(x, y, w, h, fmt, type, px);
}

// PVRTC (iOS-only on most hardware): store as RGBA8 and decode uploads.
static int is_pvrtc(GLenum f) { return f >= 0x8C00 && f <= 0x8C03; }
static void GL_APIENTRY wrap_glTexStorage2D_any(GLenum t, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h) {
    trace_tex("TexStorage2D", (GLint)ifmt, 0, 0, w, h);
    if (is_pvrtc(ifmt) || is_bgra(ifmt)) ifmt = GL_RGBA8;
    const Shrunk *s = t == GL_TEXTURE_2D ? shrunk_size(w, h) : NULL;
    if (s) {
        GLsizei most = 1;
        for (GLsizei n = s->w > s->h ? s->w : s->h; n > 1; n >>= 1) most++;
        glTexStorage2D(t, levels < most ? levels : most, ifmt, s->w, s->h);
        return;
    }
    glTexStorage2D(t, levels, ifmt, w, h);
}
static void GL_APIENTRY wrap_glCompressedTexImage2D(GLenum t, GLint lvl, GLenum ifmt, GLsizei w, GLsizei h, GLint border,
                                                    GLsizei size, const void *data) {
    if (t == GL_TEXTURE_2D && lvl == 0) shrunk_forget();
    if (is_pvrtc(ifmt)) {
        uint8_t *px = pvrtc_decode(data, (size_t)size, w, h, ifmt);
        glTexImage2D(t, lvl, GL_RGBA, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, px);
        free(px);
        return;
    }
    glCompressedTexImage2D(t, lvl, ifmt, w, h, border, size, data);
}
static void GL_APIENTRY wrap_glCompressedTexSubImage2D(GLenum t, GLint lvl, GLint x, GLint y, GLsizei w, GLsizei h,
                                                       GLenum fmt, GLsizei size, const void *data) {
    if (is_pvrtc(fmt)) {
        uint8_t *px = pvrtc_decode(data, (size_t)size, w, h, fmt);
        if (px) glTexSubImage2D(t, lvl, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        else LOGW("PVRTC decode failed (%dx%d fmt %#x size %d)", w, h, fmt, size);
        free(px);
        return;
    }
    glCompressedTexSubImage2D(t, lvl, x, y, w, h, fmt, size, data);
}

// Shader diagnostics: log compile/link failures with the source.
static void GL_APIENTRY wrap_glCompileShader(GLuint sh) {
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "", src[8192] = "";
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        glGetShaderSource(sh, sizeof src, NULL, src);
        LOGE("shader %u compile FAILED: %s\n--- source ---\n%s", sh, log, src);
    }
}
static void GL_APIENTRY wrap_glLinkProgram(GLuint p) {
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        LOGE("program %u link FAILED: %s", p, log);
    }
}

static void *g_gles_lib;
void *eagl_get_proc(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "glDiscardFramebufferEXT")) return (void *)wrap_glDiscardFramebufferEXT;
    if (!strcmp(name, "glGetString")) return (void *)wrap_glGetString;
    if (!strcmp(name, "glTexImage2D")) return (void *)wrap_glTexImage2D;
    if (!strcmp(name, "glCompileShader")) return (void *)wrap_glCompileShader;
    if (!strcmp(name, "glLinkProgram")) return (void *)wrap_glLinkProgram;
    if (!strcmp(name, "glTexSubImage2D")) return (void *)wrap_glTexSubImage2D;
    if (!strcmp(name, "glTexStorage2D")) return (void *)wrap_glTexStorage2D_any;
    if (!strcmp(name, "glCompressedTexImage2D")) return (void *)wrap_glCompressedTexImage2D;
    if (!strcmp(name, "glCompressedTexSubImage2D")) return (void *)wrap_glCompressedTexSubImage2D;
    if (!strcmp(name, "glRenderbufferStorage")) return (void *)wrap_glRenderbufferStorage;
    if (!strcmp(name, "glReadPixels")) return (void *)wrap_glReadPixels;
    // Desktop-GL / DSA entry points: report unavailable so the game uses ES paths.
    if (strstr(name, "ARB") || strstr(name, "Named") || strstr(name, "Texture") == name + 2 ||
        (!strncmp(name, "glCreate", 8) && strcmp(name, "glCreateProgram") && strcmp(name, "glCreateShader")))
        return NULL;
    void *p = NULL;
    if (!g_gles_lib) {
        g_gles_lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_gles_lib) g_gles_lib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_gles_lib) g_gles_lib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    }
    if (g_gles_lib) p = dlsym(g_gles_lib, name);
    if (!p) p = plat_gl_get_proc(name);
    return p;
}

// ---------------------------------------------------------------------------
void fw_quartz_init(void) {
    cls_CALayer = rt_define_class("CALayer", cls_NSObject, sizeof(LayerObj));
    Class l = cls_CALayer;
    M(l, "init", layer_init); M(l, "setFrame:", layer_setFrame); M(l, "frame", layer_frame);
    M(l, "bounds", layer_bounds); M(l, "setContentsScale:", layer_setContentsScale);
    M(l, "contentsScale", layer_contentsScale); M(l, "setOpaque:", layer_setOpaque);
    M(l, "setNeedsDisplay", layer_noop1); M(l, "setMagnificationFilter:", layer_noop1);
    M(l, "setMinificationFilter:", layer_noop1);
    cls_CAEAGLLayer = rt_define_class("CAEAGLLayer", cls_CALayer, sizeof(LayerObj));
    M(cls_CAEAGLLayer, "setDrawableProperties:", layer_setDrawableProperties);
    M(cls_CAEAGLLayer, "drawableProperties", layer_drawableProperties);
    M(cls_CAEAGLLayer, "setPresentsWithTransaction:", layer_noop1);

    cls_CADisplayLink = rt_define_class("CADisplayLink", cls_NSObject, sizeof(LinkObj));
    Class d = cls_CADisplayLink;
    CM(d, "displayLinkWithTarget:selector:", link_withTarget); M(d, "addToRunLoop:forMode:", link_addToRunLoop);
    M(d, "removeFromRunLoop:forMode:", link_invalidate); M(d, "invalidate", link_invalidate);
    M(d, "setPaused:", link_setPaused); M(d, "isPaused", link_isPaused);
    M(d, "setPreferredFramesPerSecond:", link_setPreferredFPS); M(d, "setFrameInterval:", link_setFrameInterval);
    M(d, "timestamp", link_timestamp); M(d, "duration", link_duration); M(d, "targetTimestamp", link_targetTimestamp);
}

void fw_eagl_init(void) {
    pthread_key_create(&g_cur_ctx_key, NULL);
    cls_EAGLSharegroup = rt_define_class("EAGLSharegroup", cls_NSObject, sizeof(struct objc_object));
    cls_EAGLContext = rt_define_class("EAGLContext", cls_NSObject, sizeof(CtxObj));
    Class c = cls_EAGLContext;
    M(c, "initWithAPI:", ctx_initWithAPI); M(c, "initWithAPI:sharegroup:", ctx_initWithAPI_share);
    M(c, "API", ctx_API); M(c, "sharegroup", ctx_sharegroup);
    CM(c, "setCurrentContext:", ctx_setCurrentContext); CM(c, "currentContext", ctx_currentContext);
    M(c, "renderbufferStorage:fromDrawable:", ctx_renderbufferStorage);
    M(c, "presentRenderbuffer:", ctx_presentRenderbuffer);
}

// EAGL constant strings
id g_kEAGLDrawablePropertyColorFormat, g_kEAGLDrawablePropertyRetainedBacking, g_kEAGLColorFormatRGBA8;
void fw_eagl_constants(void) {
    g_kEAGLDrawablePropertyColorFormat = ns_str_new("EAGLDrawablePropertyColorFormat");
    g_kEAGLDrawablePropertyRetainedBacking = ns_str_new("EAGLDrawablePropertyRetainedBacking");
    g_kEAGLColorFormatRGBA8 = ns_str_new("EAGLColorFormatRGBA8");
}
