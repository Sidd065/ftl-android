// AAudio output driving the game's RemoteIO render callback.
#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "platform.h"
#include "port_log.h"

static AAudioStream *g_stream;
static plat_audio_cb g_cb;
static void *g_ud;
static int g_rate = 44100, g_channels = 2;
static volatile bool g_paused = true;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_native_rate;

static aaudio_data_callback_result_t data_cb(AAudioStream *s, void *ud, void *audio, int32_t frames) {
    (void)s; (void)ud;
    if (g_paused || !g_cb) {
        memset(audio, 0, (size_t)frames * g_channels * 2);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    g_cb(g_ud, (int16_t *)audio, frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static bool open_stream(void);
static void *restart_thread(void *p) {
    (void)p;
    usleep(200 * 1000);
    pthread_mutex_lock(&g_lock);
    if (g_stream) {
        AAudioStream_close(g_stream);
        g_stream = NULL;
    }
    open_stream();
    pthread_mutex_unlock(&g_lock);
    return NULL;
}
static void error_cb(AAudioStream *s, void *ud, aaudio_result_t err) {
    (void)s; (void)ud;
    LOGW("AAudio error %d (%s); restarting stream", err, AAudio_convertResultToText(err));
    pthread_t th;
    pthread_create(&th, NULL, restart_thread, NULL);
    pthread_detach(th);
}

static bool open_stream(void) {
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, g_channels);
    AAudioStreamBuilder_setSampleRate(b, g_rate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    // API 28+ setters, looked up at runtime so we still run on Android 8.0.
    static void (*set_usage)(AAudioStreamBuilder *, int32_t), (*set_content)(AAudioStreamBuilder *, int32_t);
    static int looked;
    if (!looked) {
        looked = 1;
        void *h = dlopen("libaaudio.so", RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            set_usage = (void (*)(AAudioStreamBuilder *, int32_t))dlsym(h, "AAudioStreamBuilder_setUsage");
            set_content = (void (*)(AAudioStreamBuilder *, int32_t))dlsym(h, "AAudioStreamBuilder_setContentType");
        }
    }
    if (set_usage) set_usage(b, 14 /* AAUDIO_USAGE_GAME */);
    if (set_content) set_content(b, 2 /* AAUDIO_CONTENT_TYPE_MUSIC */);
    AAudioStreamBuilder_setDataCallback(b, data_cb, NULL);
    AAudioStreamBuilder_setErrorCallback(b, error_cb, NULL);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGE("AAudio open failed: %s", AAudio_convertResultToText(r));
        g_stream = NULL;
        return false;
    }
    // Keep latency modest but avoid underruns: two bursts of buffering.
    int32_t burst = AAudioStream_getFramesPerBurst(g_stream);
    AAudioStream_setBufferSizeInFrames(g_stream, burst * 4);
    LOGI("AAudio stream: %d Hz, %d ch, burst %d", AAudioStream_getSampleRate(g_stream),
         AAudioStream_getChannelCount(g_stream), burst);
    AAudioStream_requestStart(g_stream);
    return true;
}

bool plat_audio_open(int rate, int channels, plat_audio_cb cb, void *ud) {
    pthread_mutex_lock(&g_lock);
    g_cb = cb;
    g_ud = ud;
    g_rate = rate;
    g_channels = channels;
    bool ok = g_stream || open_stream();
    pthread_mutex_unlock(&g_lock);
    return ok;
}
void plat_audio_pause(bool paused) { g_paused = paused; }
int plat_audio_native_rate(void) { return g_native_rate ? g_native_rate : 48000; }
