// Test-harness platform for aarch64 Linux (run under qemu-user on the dev box).
// Headless: EGL surfaceless (Mesa llvmpipe) with an FBO standing in for the
// window. Input/screenshot commands arrive on a FIFO; audio goes to a WAV file.
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "../fw/fw.h"
#include "../fw/present.h"
#include "../port_main.h"
#include "../ux/ux.h"
#include "platform.h"
#include "port_log.h"

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLConfig g_cfg;
static int g_sw = 2400, g_sh = 1080;
static int g_cmd_fd = -1;
static char g_cmd_buf[4096];
static size_t g_cmd_len;
static char g_pending_shot[512];
static pthread_mutex_t g_shot_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_frames;
static double g_quit_at;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---------------------------------------------------------------------------
// PNG writer (RGBA8 input, GL bottom-up rows flipped, RGB output)
static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8] = {len >> 24, len >> 16, len >> 8, len, type[0], type[1], type[2], type[3]};
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t crc = (uint32_t)crc32(0, (const Bytef *)type, 4);
    if (len) crc = (uint32_t)crc32(crc, data, len);
    uint8_t c[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
    fwrite(c, 1, 4, f);
}
static void write_png(const char *path, const uint8_t *rgba, int w, int h) {
    size_t raw_len = (size_t)(w * 3 + 1) * h;
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (w * 3 + 1);
        row[0] = 0;
        const uint8_t *src = rgba + (size_t)(h - 1 - y) * w * 4;
        for (int x = 0; x < w; x++) {
            row[1 + x * 3] = src[x * 4];
            row[2 + x * 3] = src[x * 4 + 1];
            row[3 + x * 3] = src[x * 4 + 2];
        }
    }
    uLongf clen = compressBound(raw_len);
    uint8_t *comp = malloc(clen);
    compress2(comp, &clen, raw, raw_len, 6);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        uint8_t ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", comp, (uint32_t)clen);
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
        LOGI("screenshot -> %s", path);
    }
    free(raw);
    free(comp);
}

// ---------------------------------------------------------------------------
// GL: surfaceless EGL; the "window" is an FBO in the presenting context.
static GLuint g_win_fbo, g_win_rb;

static void gl_debug_cb(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei len, const GLchar *msg,
                        const void *ud) {
    (void)source; (void)id; (void)len; (void)ud;
    if (type != 0x824C && severity != 0x9146) return;  // GL_DEBUG_TYPE_ERROR / SEVERITY_HIGH
    // log each distinct message once (backtrace only with FTL_GLDEBUG=2)
    static char seen[64][160];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (!strncmp(seen[i], msg, 159)) return;
    if (nseen < 64) strncpy(seen[nseen++], msg, 159);
    LOGE("GL debug: %s", msg);
    if (getenv("FTL_GLDEBUG")[0] == '2') {
        uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
        port_backtrace_from((uintptr_t)gl_debug_cb, (uintptr_t)__builtin_return_address(0), fp, "gl error");
    }
}

void *plat_gl_create_context(int es_major, void *share) {
    EGLint dbg = getenv("FTL_GLDEBUG") ? EGL_TRUE : EGL_FALSE;
    EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, es_major, EGL_CONTEXT_OPENGL_DEBUG, dbg, EGL_NONE};
    EGLContext c = eglCreateContext(g_dpy, g_cfg, share ? (EGLContext)share : EGL_NO_CONTEXT, attrs);
    if (c == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext(ES%d) failed: %#x", es_major, eglGetError());
        return NULL;
    }
    return c;
}
bool plat_gl_make_current(void *ctx) {
    if (!ctx) return eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    bool ok = eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, (EGLContext)ctx);
    if (ok) {
        LOGI("GL: %s / %s / %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
        if (getenv("FTL_GLDEBUG")) {
            typedef void (*cbfn)(void (*)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *), const void *);
            cbfn f = (cbfn)eglGetProcAddress("glDebugMessageCallback");
            if (f) {
                f(gl_debug_cb, NULL);
                glEnable(0x8242);  // GL_DEBUG_OUTPUT_SYNCHRONOUS
            }
        }
    }
    return ok;
}
unsigned plat_gl_window_fbo(void) {
    if (!g_win_fbo) {
        glGenFramebuffers(1, &g_win_fbo);
        glGenRenderbuffers(1, &g_win_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, g_win_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, g_sw, g_sh);
        glBindFramebuffer(GL_FRAMEBUFFER, g_win_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_win_rb);
    }
    return g_win_fbo;
}
static void capture_post(int sw, int sh) {
    char path[512] = "";
    pthread_mutex_lock(&g_shot_lock);
    if (g_pending_shot[0]) {
        strcpy(path, g_pending_shot);
        g_pending_shot[0] = 0;
    }
    pthread_mutex_unlock(&g_shot_lock);
    if (!path[0]) return;
    uint8_t *px = malloc((size_t)sw * sh * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_win_fbo);
    glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    write_png(path, px, sw, sh);
    free(px);
}
void plat_gl_present(void) {
    g_frames++;
    static double last;
    double t = now_s();
    if (t - last > 5.0) {
        if (last) LOGI("harness: %d frames presented", g_frames);
        last = t;
    }
}
bool plat_gl_has_surface(void) { return true; }
void plat_gl_surface_size(int *w, int *h) {
    *w = g_sw;
    *h = g_sh;
}
void *plat_gl_get_proc(const char *name) { return (void *)eglGetProcAddress(name); }

// ---------------------------------------------------------------------------
// audio: pull from the game at real-time rate, optionally save to WAV
static plat_audio_cb g_acb;
static void *g_aud;
static int g_arate = 44100;
static volatile bool g_apaused = true;
static FILE *g_wav;
static long g_wav_bytes;
static void wav_header(FILE *f, int rate, long bytes) {
    uint32_t v;
    uint16_t s;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    v = (uint32_t)(36 + bytes);
    fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16;
    fwrite(&v, 4, 1, f);
    s = 1;
    fwrite(&s, 2, 1, f);
    s = 2;
    fwrite(&s, 2, 1, f);
    v = (uint32_t)rate;
    fwrite(&v, 4, 1, f);
    v = (uint32_t)rate * 4;
    fwrite(&v, 4, 1, f);
    s = 4;
    fwrite(&s, 2, 1, f);
    s = 16;
    fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f);
    v = (uint32_t)bytes;
    fwrite(&v, 4, 1, f);
    fseek(f, 0, SEEK_END);
}
static void *audio_thread(void *u) {
    (void)u;
    int16_t buf[1024 * 2];
    double t0 = now_s();
    long frames_done = 0;
    for (;;) {
        long due = (long)((now_s() - t0) * g_arate);
        if (frames_done + 1024 > due) {
            usleep(5000);
            continue;
        }
        if (g_apaused || !g_acb) memset(buf, 0, sizeof buf);
        else g_acb(g_aud, buf, 1024);
        frames_done += 1024;
        if (g_wav && g_wav_bytes < 120L * g_arate * 4) {
            fwrite(buf, 1, sizeof buf, g_wav);
            g_wav_bytes += (long)sizeof buf;
        }
    }
    return NULL;
}
bool plat_audio_open(int rate, int channels, plat_audio_cb cb, void *ud) {
    (void)channels;
    g_acb = cb;
    g_aud = ud;
    g_arate = rate;
    const char *wav = getenv("FTL_WAV");
    if (wav && !g_wav) {
        g_wav = fopen(wav, "wb");
        if (g_wav) wav_header(g_wav, rate, 0);
    }
    static pthread_t th;
    if (!th) pthread_create(&th, NULL, audio_thread, NULL);
    return true;
}
void plat_audio_pause(bool p) { g_apaused = p; }
int plat_audio_native_rate(void) { return 44100; }

// ---------------------------------------------------------------------------
// UI services
static char g_text_reply[256] = "Kestrel";
void plat_request_text_input(const char *prompt, const char *initial) {
    LOGI("harness: text input prompt='%s' initial='%s' -> replying '%s'", prompt ? prompt : "", initial, g_text_reply);
    fw_text_input_result(g_text_reply[0] ? g_text_reply : initial, true);
}
void plat_cancel_text_input(void) {}
void plat_show_alert(const char *title, const char *msg, const char **b, int n) {
    LOGI("harness: ALERT '%s': %s (%d buttons, first '%s')", title, msg, n, n ? b[0] : "");
    fw_alert_result(0);
}
void plat_open_url(const char *url) { LOGI("harness: open url %s", url); }
void plat_keep_screen_on(bool on) { (void)on; }
void port_report_fatal(const char *msg) { LOGE("harness: fatal: %s", msg); }
void port_on_game_exit(int code) {
    LOGI("harness: game exit %d", code);
    if (g_wav) {
        wav_header(g_wav, g_arate, g_wav_bytes);
        fclose(g_wav);
        g_wav = NULL;
    }
}
void plat_request_quit(void) {
    LOGI("harness: quit confirmation requested (auto-confirming)");
    ux_quit();
}

// ---------------------------------------------------------------------------
// command FIFO (surface pixel coords):
//   tap x y | drag x1 y1 x2 y2 ms | down id x y | move id x y | up id x y |
//   shot file.png | text str | quit
typedef struct {
    double at;
    int phase;
    long id;
    double x, y;
} pending_touch;
static pending_touch g_pt[64];
static int g_npt;
static void queue_touch(double at, int phase, long id, double x, double y) {
    if (g_npt < 64) g_pt[g_npt++] = (pending_touch){at, phase, id, x, y};
}
static void touch_now(int phase, long id, double sx, double sy) { ux_handle_touch(phase, id, (float)sx, (float)sy, now_s()); }
static void handle_cmd(char *line) {
    char a[512];
    double x, y, x2, y2;
    long id;
    int ms;
    if (sscanf(line, "tap %lf %lf", &x, &y) == 2) {
        double t = now_s();
        queue_touch(t, TOUCH_BEGAN, 99, x, y);
        queue_touch(t + 0.15, TOUCH_ENDED, 99, x, y);
    } else if (sscanf(line, "drag %lf %lf %lf %lf %d", &x, &y, &x2, &y2, &ms) == 5) {
        double t = now_s();
        queue_touch(t, TOUCH_BEGAN, 98, x, y);
        for (int i = 1; i <= 10; i++)
            queue_touch(t + ms / 1000.0 * i / 10, TOUCH_MOVED, 98, x + (x2 - x) * i / 10, y + (y2 - y) * i / 10);
        queue_touch(t + ms / 1000.0 + 0.05, TOUCH_ENDED, 98, x2, y2);
    } else if (sscanf(line, "down %ld %lf %lf", &id, &x, &y) == 3) {
        touch_now(TOUCH_BEGAN, id, x, y);
    } else if (sscanf(line, "move %ld %lf %lf", &id, &x, &y) == 3) {
        touch_now(TOUCH_MOVED, id, x, y);
    } else if (sscanf(line, "up %ld %lf %lf", &id, &x, &y) == 3) {
        touch_now(TOUCH_ENDED, id, x, y);
    } else if (sscanf(line, "shot %511s", a) == 1) {
        pthread_mutex_lock(&g_shot_lock);
        strcpy(g_pending_shot, a);
        pthread_mutex_unlock(&g_shot_lock);
    } else if (!strncmp(line, "text ", 5)) {
        strncpy(g_text_reply, line + 5, sizeof g_text_reply - 1);
    } else if (!strcmp(line, "quit")) {
        // Go through the iOS lifecycle so the game saves settings/progress.
        LOGI("harness: quit requested (resign active -> terminate)");
        fw_app_will_resign_active();
        fw_app_will_terminate();
        g_quit_at = now_s() + 8.0;
    } else if (!strcmp(line, "pause")) {
        fw_app_will_resign_active();
    } else if (!strcmp(line, "resume")) {
        fw_app_did_become_active();
    } else if (*line) {
        LOGW("harness: unknown command '%s'", line);
    }
}
void plat_pump_events(void) {
    double t = now_s();
    if (g_quit_at > 0 && t >= g_quit_at) {
        if (g_wav) {
            wav_header(g_wav, g_arate, g_wav_bytes);
            fclose(g_wav);
            g_wav = NULL;
        }
        LOGI("harness: exiting");
        _exit(0);
    }
    for (int i = 0; i < g_npt; i++) {
        if (g_pt[i].at <= t) {
            pending_touch p = g_pt[i];
            memmove(g_pt + i, g_pt + i + 1, (size_t)(g_npt - i - 1) * sizeof(pending_touch));
            g_npt--;
            touch_now(p.phase, p.id, p.x, p.y);
            break;  // one per pump so began/ended land in separate iterations
        }
    }
    ux_tick();
    if (g_cmd_fd < 0) return;
    for (;;) {
        ssize_t n = read(g_cmd_fd, g_cmd_buf + g_cmd_len, sizeof g_cmd_buf - 1 - g_cmd_len);
        if (n <= 0) break;
        g_cmd_len += (size_t)n;
    }
    g_cmd_buf[g_cmd_len] = 0;
    char *nl;
    while ((nl = strchr(g_cmd_buf, '\n'))) {
        *nl = 0;
        handle_cmd(g_cmd_buf);
        size_t used = (size_t)(nl + 1 - g_cmd_buf);
        memmove(g_cmd_buf, nl + 1, g_cmd_len - used + 1);
        g_cmd_len -= used;
    }
}
void plat_wake_main(void) {}
void plat_main_thread_started(void) {}

// ---------------------------------------------------------------------------
static void on_signal(int sig) {
    LOGE("harness: signal %d", sig);
    if (g_wav) {
        wav_header(g_wav, g_arate, g_wav_bytes);
        fflush(g_wav);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

int main(int argc, char **argv) {
    const char *app = argc > 1 ? argv[1] : "./FTL.app";
    const char *data = argc > 2 ? argv[2] : "./ftl-data";
    if (getenv("FTL_SIZE")) sscanf(getenv("FTL_SIZE"), "%dx%d", &g_sw, &g_sh);
    signal(SIGINT, on_signal);
    port_install_crash_handler();

    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj, min;
    if (!eglInitialize(g_dpy, &maj, &min)) {
        LOGE("eglInitialize failed");
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cattr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLint nc = 0;
    if (!eglChooseConfig(g_dpy, cattr, &g_cfg, 1, &nc) || nc < 1) {
        EGLint cattr2[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
        eglChooseConfig(g_dpy, cattr2, &g_cfg, 1, &nc);
    }
    LOGI("harness: EGL %d.%d, surface %dx%d", maj, min, g_sw, g_sh);

    const char *fifo = getenv("FTL_CMD") ? getenv("FTL_CMD") : "/tmp/ftl_cmd";
    mkfifo(fifo, 0644);
    g_cmd_fd = open(fifo, O_RDONLY | O_NONBLOCK);
    present_set_post_hook(capture_post);

    float layout[8] = {0, 0, 0, 0, 400, getenv("FTL_RENDER_H") ? (float)atoi(getenv("FTL_RENDER_H")) : 768};
    if (getenv("FTL_INSETS")) sscanf(getenv("FTL_INSETS"), "%f,%f,%f,%f", &layout[0], &layout[1], &layout[2], &layout[3]);
    ux_configure(g_sw, g_sh, 60, layout);

    char bin[600], docs[600], cache[600];
    snprintf(bin, sizeof bin, "%s/FTL", app);
    snprintf(docs, sizeof docs, "%s/Documents", data);
    snprintf(cache, sizeof cache, "%s/Caches", data);
    mkdir(data, 0755);
    port_config cfg = {bin, app, docs, cache, getenv("FTL_HWMACHINE"), getenv("FTL_VERBOSE") ? 1 : 0};
    return port_run_game(&cfg);
}
