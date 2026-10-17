// Android platform layer: JNI bridge to FTLActivity, EGL window surface
// management (the game thread owns the GL context), input/lifecycle queue.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../fw/fw.h"
#include "../fw/present.h"
#include "../port_main.h"
#include "../ux/ux.h"
#include "platform.h"
#include "port_log.h"

static JavaVM *g_vm;
static jobject g_activity;  // global ref
static jclass g_activity_cls;

// ---------------------------------------------------------------------------
// EGL
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLConfig g_cfg;
static EGLSurface g_pbuffer = EGL_NO_SURFACE;
static EGLContext g_ctx = EGL_NO_CONTEXT;      // the game's context
static pthread_mutex_t g_surf_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_surf_cond = PTHREAD_COND_INITIALIZER;
static ANativeWindow *g_pending_window;        // set by UI thread, consumed by GL thread
static int g_window_gen, g_bound_gen;          // generations of window changes
static EGLSurface g_win_surface = EGL_NO_SURFACE;
static int g_surface_w, g_surface_h;
static int g_release_requested;
static bool g_has_window;
static pthread_t g_gl_thread;
static bool g_gl_thread_set;

static void egl_init_once(void) {
    if (g_dpy != EGL_NO_DISPLAY) return;
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(g_dpy, NULL, NULL);
    EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
                      EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE};
    EGLint n = 0;
    eglChooseConfig(g_dpy, attrs, &g_cfg, 1, &n);
    if (n < 1) {
        EGLint attrs2[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
        eglChooseConfig(g_dpy, attrs2, &g_cfg, 1, &n);
    }
    EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    g_pbuffer = eglCreatePbufferSurface(g_dpy, g_cfg, pb);
    LOGI("EGL ready (%d configs), pbuffer %p", n, g_pbuffer);
}

void *plat_gl_create_context(int es_major, void *share) {
    egl_init_once();
    EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, es_major, EGL_NONE};
    EGLContext c = eglCreateContext(g_dpy, g_cfg, share ? (EGLContext)share : EGL_NO_CONTEXT, attrs);
    if (c == EGL_NO_CONTEXT) LOGE("eglCreateContext(ES%d) failed: %#x", es_major, eglGetError());
    return c == EGL_NO_CONTEXT ? NULL : c;
}

// GL thread: adopt the latest window (or fall back to the pbuffer).
static void sync_surface_locked(void) {
    if (g_bound_gen == g_window_gen && !g_release_requested) return;
    if (g_win_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(g_dpy, g_pbuffer, g_pbuffer, g_ctx);
        eglDestroySurface(g_dpy, g_win_surface);
        g_win_surface = EGL_NO_SURFACE;
    }
    if (g_pending_window && !g_release_requested) {
        g_win_surface = eglCreateWindowSurface(g_dpy, g_cfg, g_pending_window, NULL);
        if (g_win_surface == EGL_NO_SURFACE) LOGE("eglCreateWindowSurface failed: %#x", eglGetError());
    }
    EGLSurface s = g_win_surface != EGL_NO_SURFACE ? g_win_surface : g_pbuffer;
    if (!eglMakeCurrent(g_dpy, s, s, g_ctx)) LOGE("eglMakeCurrent failed: %#x", eglGetError());
    if (g_win_surface != EGL_NO_SURFACE) {
        eglQuerySurface(g_dpy, g_win_surface, EGL_WIDTH, &g_surface_w);
        eglQuerySurface(g_dpy, g_win_surface, EGL_HEIGHT, &g_surface_h);
        eglSwapInterval(g_dpy, 1);
        LOGI("window surface bound: %dx%d", g_surface_w, g_surface_h);
    }
    g_bound_gen = g_window_gen;
    g_release_requested = 0;
    pthread_cond_broadcast(&g_surf_cond);
}

// Wait hook (see darwin_set_wait_hook): while the game thread is parked, e.g.
// suspended in the background, it still lets go of a window being destroyed.
void darwin_set_wait_hook(void (*fn)(void));
static void gl_wait_service(void) {
    if (!g_gl_thread_set || !pthread_equal(pthread_self(), g_gl_thread) || !g_release_requested) return;
    pthread_mutex_lock(&g_surf_lock);
    if (g_release_requested && g_win_surface != EGL_NO_SURFACE && g_ctx != EGL_NO_CONTEXT) {
        eglMakeCurrent(g_dpy, g_pbuffer, g_pbuffer, g_ctx);
        eglDestroySurface(g_dpy, g_win_surface);
        g_win_surface = EGL_NO_SURFACE;
        g_bound_gen = -1;  // rebind whatever window comes next
        LOGI("window surface released by the parked game thread");
        pthread_cond_broadcast(&g_surf_cond);
    }
    pthread_mutex_unlock(&g_surf_lock);
}

bool plat_gl_make_current(void *ctx) {
    egl_init_once();
    if (!ctx) return eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    pthread_mutex_lock(&g_surf_lock);
    g_gl_thread = pthread_self();
    g_gl_thread_set = true;
    darwin_set_wait_hook(gl_wait_service);
    g_ctx = (EGLContext)ctx;
    g_bound_gen = -1;  // force (re)binding on this thread
    sync_surface_locked();
    pthread_mutex_unlock(&g_surf_lock);
    LOGI("GL: %s / %s / %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return true;
}

bool plat_gl_has_surface(void) {
    pthread_mutex_lock(&g_surf_lock);
    if (g_ctx != EGL_NO_CONTEXT) sync_surface_locked();
    bool r = g_win_surface != EGL_NO_SURFACE;
    pthread_mutex_unlock(&g_surf_lock);
    return r;
}
void plat_gl_surface_size(int *w, int *h) {
    *w = g_surface_w;
    *h = g_surface_h;
}
static int64_t g_last_present_ms;  // CLOCK_MONOTONIC ms of the last frame shown
static bool g_foreground;          // between nativeResume and nativePause
static double now_s(void);
static void mark_presented(void) { __atomic_store_n(&g_last_present_ms, (int64_t)(now_s() * 1000), __ATOMIC_RELAXED); }

void plat_gl_present(void) {
    mark_presented();
    if (g_win_surface != EGL_NO_SURFACE && !eglSwapBuffers(g_dpy, g_win_surface)) {
        EGLint e = eglGetError();
        LOGW("eglSwapBuffers failed: %#x", e);
        if (e == EGL_BAD_SURFACE || e == EGL_BAD_NATIVE_WINDOW || e == EGL_CONTEXT_LOST) {
            pthread_mutex_lock(&g_surf_lock);
            g_bound_gen = -1;
            pthread_mutex_unlock(&g_surf_lock);
        }
    }
}
unsigned plat_gl_window_fbo(void) { return 0; }
void *plat_gl_get_proc(const char *name) { return (void *)eglGetProcAddress(name); }

// ---------------------------------------------------------------------------
// event queue (UI thread -> game main thread)
enum { EV_TOUCH, EV_PAUSE, EV_RESUME, EV_LOWMEM, EV_TEXT, EV_ALERT, EV_BACK, EV_KEY, EV_MOUSE, EV_QUIT,
       EV_PAD_CONN, EV_PAD_BUTTON, EV_PAD_AXES };
typedef struct {
    int type;
    int phase;
    long id;
    float x, y;
    double t;
    char *text;
    int ok;
    float ax[8];
} pevent;
#define QCAP 512
static pevent g_q[QCAP];
static int g_qh, g_qt;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;
static void q_push(pevent e) {
    pthread_mutex_lock(&g_qlock);
    int nt = (g_qt + 1) % QCAP;
    if (nt != g_qh) {
        g_q[g_qt] = e;
        g_qt = nt;
    }
    pthread_mutex_unlock(&g_qlock);
    plat_wake_main();
}
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

void plat_pump_events(void) {
    for (;;) {
        pthread_mutex_lock(&g_qlock);
        if (g_qh == g_qt) {
            pthread_mutex_unlock(&g_qlock);
            break;
        }
        pevent e = g_q[g_qh];
        g_qh = (g_qh + 1) % QCAP;
        pthread_mutex_unlock(&g_qlock);
        switch (e.type) {
        case EV_TOUCH: ux_handle_touch(e.phase, e.id, e.x, e.y, e.t); break;
        case EV_PAUSE: fw_app_will_resign_active(); break;
        case EV_RESUME: fw_app_did_become_active(); break;
        case EV_LOWMEM: fw_app_memory_warning(); break;
        case EV_TEXT:
            fw_text_input_result(e.text, e.ok);
            free(e.text);
            break;
        case EV_ALERT: fw_alert_result(e.ok); break;
        case EV_BACK: ux_handle_back(); break;
        case EV_KEY: ux_handle_key((int)e.id, e.phase != 0); break;
        case EV_MOUSE: ux_handle_mouse(e.phase, e.x, e.y, (float)e.t); break;
        case EV_QUIT: ux_quit(); break;
        case EV_PAD_CONN:
            ux_pad_connected(e.phase != 0, e.text);
            free(e.text);
            break;
        case EV_PAD_BUTTON: ux_pad_button((int)e.id, e.phase != 0); break;
        case EV_PAD_AXES: ux_pad_axes(e.ax); break;
        }
    }
    ux_tick();
}
void plat_wake_main(void) {}
void plat_main_thread_started(void) {}

// ---------------------------------------------------------------------------
// Java calls
static JNIEnv *jenv(void) {
    JNIEnv *env = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
        (*g_vm)->AttachCurrentThread(g_vm, &env, NULL);
    return env;
}
static void call_void(const char *name, const char *sig, ...) {
    JNIEnv *env = jenv();
    jmethodID m = (*env)->GetMethodID(env, g_activity_cls, name, sig);
    if (!m) { (*env)->ExceptionClear(env); LOGE("missing Java method %s", name); return; }
    va_list ap;
    va_start(ap, sig);
    (*env)->CallVoidMethodV(env, g_activity, m, ap);
    va_end(ap);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}
static jstring jstr(const char *s) { return (*jenv())->NewStringUTF(jenv(), s ? s : ""); }

void plat_request_text_input(const char *prompt, const char *initial) {
    call_void("requestTextInput", "(Ljava/lang/String;Ljava/lang/String;)V", jstr(prompt), jstr(initial));
}
void plat_cancel_text_input(void) { call_void("cancelTextInput", "()V"); }
void plat_show_alert(const char *title, const char *msg, const char **buttons, int n) {
    JNIEnv *env = jenv();
    jobjectArray arr = (*env)->NewObjectArray(env, n, (*env)->FindClass(env, "java/lang/String"), NULL);
    for (int i = 0; i < n; i++) (*env)->SetObjectArrayElement(env, arr, i, jstr(buttons[i]));
    call_void("showAlert", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)V", jstr(title), jstr(msg), arr);
}
void plat_open_url(const char *url) { call_void("openUrl", "(Ljava/lang/String;)V", jstr(url)); }
void plat_request_quit(void) { call_void("confirmQuit", "()V"); }
void plat_keep_screen_on(bool on) { (void)on; }  // the activity always keeps the screen on
void plat_haptic(int kind) { call_void("haptic", "(I)V", kind); }
void port_report_fatal(const char *msg) { call_void("showFatal", "(Ljava/lang/String;)V", jstr(msg)); }
// The game called exit() (normally after saving on terminate). Let go of the
// window, have the activity finish (it kills the process once destroyed) and
// keep this thread parked so the game does not run on; _exit as a fallback.
void port_on_game_exit(int code) {
    pthread_mutex_lock(&g_surf_lock);
    if (g_ctx != EGL_NO_CONTEXT) eglMakeCurrent(g_dpy, g_pbuffer, g_pbuffer, g_ctx);
    if (g_win_surface != EGL_NO_SURFACE) {
        eglDestroySurface(g_dpy, g_win_surface);
        g_win_surface = EGL_NO_SURFACE;
    }
    g_release_requested = 1;  // never bind a window again
    pthread_cond_broadcast(&g_surf_cond);
    pthread_mutex_unlock(&g_surf_lock);
    plat_audio_pause(true);
    call_void("onGameExit", "()V");
    sleep(3);
    LOGW("activity did not finish; exiting");
    _exit(code);
}

// ---------------------------------------------------------------------------
// Watchdog: once past loading, the game draws every frame. If it stops while on
// screen, it's stuck (e.g. loading a run saved with other mods). Say so, and mark
// the session like a crash so the next start skips the saved run. Not while the
// game starts: with big mods, setting up after its loading bar can take a while.
static void *watchdog_thread(void *p) {
    (void)p;
    bool reported = false;
    for (;;) {
        sleep(5);
        double last = __atomic_load_n(&g_last_present_ms, __ATOMIC_RELAXED) / 1000.0;
        bool stuck = g_foreground && g_has_window && last > 0 && ux_game_started() && now_s() - last > 30;
        if (stuck && !reported) {
            LOGE("watchdog: no frame for %.0f s; the game seems stuck", now_s() - last);
            port_log_mark_crash();
            call_void("onHang", "()V");
        }
        reported = stuck;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// JNI entry points
typedef struct {
    char bin[512], bundle[512], docs[512], cache[512], mods[512], hyperspace[512], logs[512];
} start_args;
static void *game_main_thread(void *p) {
    start_args *a = p;
    port_config cfg = {a->bin, a->bundle, a->docs, a->cache, "iPad8,1", 0, a->mods[0] ? a->mods : NULL,
                       a->hyperspace[0] ? a->hyperspace : NULL, a->logs};
    port_install_crash_handler();
    int r = port_run_game(&cfg);
    LOGE("port_run_game returned %d", r);
    return NULL;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_6;
}

#define JNIFN(name) Java_dev_ftlport_GameActivity_##name

JNIEXPORT void JNICALL JNIFN(nativeStart)(JNIEnv *env, jobject thiz, jstring bin, jstring bundle, jstring docs,
                                          jstring cache, jstring logs, jstring mods, jstring hyperspace, jint sw,
                                          jint sh, jint refresh, jfloatArray layout) {
    start_args *a = calloc(1, sizeof(*a));
    const char *s = (*env)->GetStringUTFChars(env, logs, NULL);
    port_log_open(s);
    strncpy(a->logs, s, 511);
    (*env)->ReleaseStringUTFChars(env, logs, s);
    g_activity = (*env)->NewGlobalRef(env, thiz);
    g_activity_cls = (*env)->NewGlobalRef(env, (*env)->GetObjectClass(env, thiz));
    s = (*env)->GetStringUTFChars(env, bin, NULL); strncpy(a->bin, s, 511); (*env)->ReleaseStringUTFChars(env, bin, s);
    s = (*env)->GetStringUTFChars(env, bundle, NULL); strncpy(a->bundle, s, 511); (*env)->ReleaseStringUTFChars(env, bundle, s);
    s = (*env)->GetStringUTFChars(env, docs, NULL); strncpy(a->docs, s, 511); (*env)->ReleaseStringUTFChars(env, docs, s);
    s = (*env)->GetStringUTFChars(env, cache, NULL); strncpy(a->cache, s, 511); (*env)->ReleaseStringUTFChars(env, cache, s);
    s = (*env)->GetStringUTFChars(env, mods, NULL); strncpy(a->mods, s, 511); (*env)->ReleaseStringUTFChars(env, mods, s);
    s = (*env)->GetStringUTFChars(env, hyperspace, NULL); strncpy(a->hyperspace, s, 511); (*env)->ReleaseStringUTFChars(env, hyperspace, s);
    float lay[8] = {0};
    jsize n = (*env)->GetArrayLength(env, layout);
    (*env)->GetFloatArrayRegion(env, layout, 0, n < 8 ? n : 8, lay);
    ux_configure(sw, sh, refresh, lay);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8 * 1024 * 1024);
    pthread_create(&th, &at, game_main_thread, a);
    pthread_attr_destroy(&at);
    g_foreground = true;
    pthread_t wd;
    if (pthread_create(&wd, NULL, watchdog_thread, NULL) == 0) pthread_detach(wd);
}

JNIEXPORT void JNICALL JNIFN(nativeAttach)(JNIEnv *env, jobject thiz) {
    // A new activity instance took over (the game keeps running in this process).
    jobject old = g_activity;
    g_activity = (*env)->NewGlobalRef(env, thiz);
    if (old) (*env)->DeleteGlobalRef(env, old);
}

JNIEXPORT void JNICALL JNIFN(nativeSurfaceChanged)(JNIEnv *env, jobject thiz, jobject surface, jint w, jint h) {
    (void)thiz;
    ANativeWindow *win = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&g_surf_lock);
    if (g_pending_window) ANativeWindow_release(g_pending_window);
    g_pending_window = win;
    g_has_window = win != NULL;
    g_release_requested = 0;  // a new window supersedes a pending release
    g_window_gen++;
    pthread_mutex_unlock(&g_surf_lock);
    LOGI("surface changed: %dx%d (%p)", w, h, (void *)win);
}

JNIEXPORT void JNICALL JNIFN(nativeSurfaceDestroyed)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    pthread_mutex_lock(&g_surf_lock);
    g_release_requested = 1;
    g_window_gen++;
    // Wait (bounded) for the GL thread to stop using the window.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    while (g_win_surface != EGL_NO_SURFACE && g_ctx != EGL_NO_CONTEXT) {
        if (pthread_cond_timedwait(&g_surf_cond, &g_surf_lock, &ts) != 0) {
            LOGW("GL thread did not release the surface in time");
            break;
        }
    }
    if (g_pending_window) {
        ANativeWindow_release(g_pending_window);
        g_pending_window = NULL;
    }
    g_has_window = false;
    pthread_mutex_unlock(&g_surf_lock);
    LOGI("surface destroyed");
}

JNIEXPORT void JNICALL JNIFN(nativeTouch)(JNIEnv *env, jobject thiz, jint phase, jint id, jfloat x, jfloat y) {
    (void)env; (void)thiz;
    q_push((pevent){EV_TOUCH, phase, id, x, y, now_s(), NULL, 0});
}
JNIEXPORT void JNICALL JNIFN(nativePause)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    g_foreground = false;
    q_push((pevent){.type = EV_PAUSE});
    plat_audio_pause(true);
}
JNIEXPORT void JNICALL JNIFN(nativeResume)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    mark_presented();  // give the game time to wake up
    g_foreground = true;
    q_push((pevent){.type = EV_RESUME});
    plat_audio_pause(false);
}
JNIEXPORT void JNICALL JNIFN(nativeLowMemory)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    long free_mb, total_mb, game_mb;
    port_memory(&free_mb, &total_mb, &game_mb);
    LOGW("memory warning: %ld MB free of %ld MB, the game holds %ld MB", free_mb, total_mb, game_mb);
    q_push((pevent){.type = EV_LOWMEM});
}
JNIEXPORT void JNICALL JNIFN(nativeBack)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    q_push((pevent){.type = EV_BACK});
}
JNIEXPORT void JNICALL JNIFN(nativeQuit)(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    q_push((pevent){.type = EV_QUIT});
}
JNIEXPORT void JNICALL JNIFN(nativePadConnected)(JNIEnv *env, jobject thiz, jboolean connected, jstring name) {
    (void)thiz;
    const char *s = name ? (*env)->GetStringUTFChars(env, name, NULL) : NULL;
    q_push((pevent){.type = EV_PAD_CONN, .phase = connected ? 1 : 0, .text = s ? strdup(s) : NULL});
    if (s) (*env)->ReleaseStringUTFChars(env, name, s);
}
JNIEXPORT void JNICALL JNIFN(nativePadButton)(JNIEnv *env, jobject thiz, jint keycode, jboolean down) {
    (void)env; (void)thiz;
    q_push((pevent){.type = EV_PAD_BUTTON, .phase = down ? 1 : 0, .id = keycode});
}
JNIEXPORT void JNICALL JNIFN(nativePadAxes)(JNIEnv *env, jobject thiz, jfloatArray axes) {
    (void)thiz;
    pevent e = {.type = EV_PAD_AXES};
    jsize n = (*env)->GetArrayLength(env, axes);
    (*env)->GetFloatArrayRegion(env, axes, 0, n < 8 ? n : 8, e.ax);
    q_push(e);
}
JNIEXPORT void JNICALL JNIFN(nativeKey)(JNIEnv *env, jobject thiz, jint keycode, jboolean down) {
    (void)env; (void)thiz;
    q_push((pevent){.type = EV_KEY, .phase = down ? 1 : 0, .id = keycode});
}
JNIEXPORT void JNICALL JNIFN(nativeMouse)(JNIEnv *env, jobject thiz, jint action, jfloat x, jfloat y, jfloat wheel) {
    (void)env; (void)thiz;
    q_push((pevent){.type = EV_MOUSE, .phase = action, .x = x, .y = y, .t = wheel});
}
JNIEXPORT void JNICALL JNIFN(nativeTextResult)(JNIEnv *env, jobject thiz, jstring text, jboolean ok) {
    (void)thiz;
    const char *s = text ? (*env)->GetStringUTFChars(env, text, NULL) : NULL;
    q_push((pevent){.type = EV_TEXT, .text = s ? strdup(s) : NULL, .ok = ok});
    if (s) (*env)->ReleaseStringUTFChars(env, text, s);
}
JNIEXPORT void JNICALL JNIFN(nativeAlertResult)(JNIEnv *env, jobject thiz, jint button) {
    (void)env; (void)thiz;
    q_push((pevent){.type = EV_ALERT, .ok = button});
}
JNIEXPORT void JNICALL JNIFN(nativeSetOption)(JNIEnv *env, jobject thiz, jstring key, jfloat value) {
    (void)thiz;
    const char *k = (*env)->GetStringUTFChars(env, key, NULL);
    ux_set_option(k, value);
    (*env)->ReleaseStringUTFChars(env, key, k);
}
