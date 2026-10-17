// Session logs players can send without adb: every log line also goes to
// <files>/logs/session-<date>.txt (the newest few sessions are kept), and the
// crash handler leaves a marker so the app can offer the log on the next start.
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "port_log.h"

#define KEEP_SESSIONS 5
#define MAX_SESSION_BYTES (8 << 20)

static int g_fd = -1;
static size_t g_bytes;
static char g_crash_path[600];

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// Keep the newest `keep` session logs (their names sort by date).
static void prune(const char *dir, int keep) {
    DIR *d = opendir(dir);
    if (!d) return;
    char *names[128];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 128)
        if (!strncmp(e->d_name, "session-", 8)) names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, n, sizeof *names, by_name);
    for (int i = 0; i < n; i++) {
        if (i < n - keep) {
            char p[600];
            snprintf(p, sizeof p, "%s/%s", dir, names[i]);
            unlink(p);
        }
        free(names[i]);
    }
}

void port_log_open(const char *dir) {
    if (g_fd >= 0 || !dir || !*dir) return;
    mkdir(dir, 0700);
    prune(dir, KEEP_SESSIONS - 1);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char name[64], path[600];
    strftime(name, sizeof name, "session-%Y%m%d-%H%M%S.txt", &tm);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(g_crash_path, sizeof g_crash_path, "%s/crashed", dir);
    g_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    char model[PROP_VALUE_MAX] = "", maker[PROP_VALUE_MAX] = "", release[PROP_VALUE_MAX] = "";
    __system_property_get("ro.product.manufacturer", maker);
    __system_property_get("ro.product.model", model);
    __system_property_get("ro.build.version.release", release);
    char date[32];
    strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tm);
    long free_mb, total_mb, game_mb;
    port_memory(&free_mb, &total_mb, &game_mb);
    LOGI("session started %s on %s %s, Android %s, %ld MB of memory (%ld MB free)", date, maker, model, release,
         total_mb, free_mb);
}

// Android closes the game without a word when the device runs out of memory, so the log
// notes how much is left: at the start, on memory warnings and with the frame rate.
void port_memory(long *free_mb, long *total_mb, long *game_mb) {
    long total = 0, avail = 0, rss = 0;
    char line[128];
    FILE *f = fopen("/proc/meminfo", "re");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "MemTotal: %ld", &total);
            sscanf(line, "MemAvailable: %ld", &avail);
        }
        fclose(f);
    }
    f = fopen("/proc/self/status", "re");
    if (f) {
        while (fgets(line, sizeof line, f)) sscanf(line, "VmRSS: %ld", &rss);
        fclose(f);
    }
    *free_mb = avail >> 10;
    *total_mb = total >> 10;
    *game_mb = rss >> 10;
}

void port_log(int prio, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    __android_log_write(prio, PORT_TAG, msg);
    if (g_fd < 0 || g_bytes > MAX_SESSION_BYTES) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    char line[1100];
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d %c %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
                     (int)(tv.tv_usec / 1000), prio >= ANDROID_LOG_ERROR ? 'E' : prio == ANDROID_LOG_WARN ? 'W' : 'I',
                     msg);
    if (n >= (int)sizeof line) {
        n = sizeof line - 1;
        line[n - 1] = '\n';
    }
    g_bytes += n;  // a soft cap, so a runaway message can't fill the storage
    if (g_bytes > MAX_SESSION_BYTES) {
        static const char cut[] = "(log cut here: too long)\n";
        n = sizeof cut - 1;
        memcpy(line, cut, n);
    }
    ssize_t w = write(g_fd, line, n);
    (void)w;
}

void port_log_mark_crash(void) {
    if (!g_crash_path[0]) return;
    int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) close(fd);
}
