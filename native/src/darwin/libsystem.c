// Darwin libSystem shim: translates the iOS C-library ABI (struct layouts,
// flag values, variadic convention, pthread object formats, Mach APIs) onto
// the host libc (bionic on Android, glibc for the Linux test harness).
#define _GNU_SOURCE
#include "libsystem.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "port_log.h"
#include "../port_main.h"

// ---------------------------------------------------------------------------
// variadic helpers: build an AAPCS64 va_list whose args all come from `stack`.
struct aapcs_va_list {
    void *stack;
    void *gr_top;
    void *vr_top;
    int gr_offs;
    int vr_offs;
};
#define MAKE_VA(ap, stackptr)                                       \
    va_list ap;                                                     \
    do {                                                            \
        struct aapcs_va_list _v = {(stackptr), NULL, NULL, 0, 0};   \
        _Static_assert(sizeof(_v) == sizeof(va_list), "va_list");  \
        memcpy(&ap, &_v, sizeof(_v));                               \
    } while (0)

int darwin_vsnprintf_stack(char *buf, size_t n, const char *fmt, void *stack) {
    MAKE_VA(ap, stack);
    return vsnprintf(buf, n, fmt, ap);
}

int darwin_sprintf_chk_impl(char *s, int flag, size_t slen, const char *fmt, void *stack) {
    (void)flag;
    MAKE_VA(ap, stack);
    int r = vsnprintf(s, slen, fmt, ap);
    if (r >= 0 && (size_t)r >= slen) port_fatal("__sprintf_chk overflow (%d >= %zu) fmt=%s", r, slen, fmt);
    return r;
}
int darwin_sprintf_impl(char *s, const char *fmt, void *stack) {
    MAKE_VA(ap, stack);
    return vsprintf(s, fmt, ap);
}
int darwin_snprintf_impl(char *s, size_t n, const char *fmt, void *stack) {
    MAKE_VA(ap, stack);
    return vsnprintf(s, n, fmt, ap);
}
int darwin_asprintf_impl(char **ret, const char *fmt, void *stack) {
    MAKE_VA(ap, stack);
    return vasprintf(ret, fmt, ap);
}
int darwin_printf_impl(const char *fmt, void *stack) {
    char buf[2048];
    MAKE_VA(ap, stack);
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    LOGI("[game] %s", buf);
    return r;
}
int darwin_fprintf_impl(FILE *f, const char *fmt, void *stack) {
    MAKE_VA(ap, stack);
    return vfprintf(f, fmt, ap);
}
int darwin_sscanf_impl(const char *s, const char *fmt, void *stack) {
    MAKE_VA(ap, stack);
    return vsscanf(s, fmt, ap);
}

// ---------------------------------------------------------------------------
// errno must behave like a real variable: the game writes its own codes
// (e.g. 8 for "I/O queue busy") and reads them back later. Share the host's
// thread-local errno; codes 1..34 mean the same on Darwin and Linux.
int *darwin___error(void) { return &errno; }

// ---------------------------------------------------------------------------
// files
#define D_O_NONBLOCK 0x0004
#define D_O_APPEND 0x0008
#define D_O_NOFOLLOW 0x0100
#define D_O_CREAT 0x0200
#define D_O_TRUNC 0x0400
#define D_O_EXCL 0x0800
#define D_O_DIRECTORY 0x100000
#define D_O_CLOEXEC 0x1000000

static int oflags_from_darwin(int f) {
    int r = f & 3;
    if (f & D_O_NONBLOCK) r |= O_NONBLOCK;
    if (f & D_O_APPEND) r |= O_APPEND;
    if (f & D_O_NOFOLLOW) r |= O_NOFOLLOW;
    if (f & D_O_CREAT) r |= O_CREAT;
    if (f & D_O_TRUNC) r |= O_TRUNC;
    if (f & D_O_EXCL) r |= O_EXCL;
    if (f & D_O_DIRECTORY) r |= O_DIRECTORY;
    if (f & D_O_CLOEXEC) r |= O_CLOEXEC;
    return r;
}
static int oflags_to_darwin(int f) {
    int r = f & 3;
    if (f & O_NONBLOCK) r |= D_O_NONBLOCK;
    if (f & O_APPEND) r |= D_O_APPEND;
    return r;
}

static int g_iotrace = -1;
static int iotrace(void) {
    if (g_iotrace < 0) g_iotrace = getenv("FTL_IOTRACE") != NULL;
    return g_iotrace;
}
int darwin_open_impl(const char *path, int flags, int mode) {
    int fd = open(path, oflags_from_darwin(flags), mode);
    if (iotrace()) LOGI("iotrace open(%s, %#x) = %d", path, flags, fd);
    return fd;
}
ssize_t darwin_pread(int fd, void *buf, size_t n, off_t off) {
    ssize_t r = pread(fd, buf, n, off);
    if (iotrace()) LOGI("iotrace pread(%d, %zu, %lld) = %zd", fd, n, (long long)off, r);
    return r;
}
ssize_t darwin_read(int fd, void *buf, size_t n) {
    ssize_t r = read(fd, buf, n);
    if (iotrace()) LOGI("iotrace read(%d, %zu) = %zd", fd, n, r);
    return r;
}
off_t darwin_lseek(int fd, off_t off, int whence) {
    off_t r = lseek(fd, off, whence);
    if (iotrace()) LOGI("iotrace lseek(%d, %lld, %d) = %lld", fd, (long long)off, whence, (long long)r);
    return r;
}

int darwin_fcntl_impl(int fd, int cmd, long arg) {
    switch (cmd) {
    case 1: return fcntl(fd, F_GETFD);
    case 2: return fcntl(fd, F_SETFD, (int)arg);
    case 3: { int r = fcntl(fd, F_GETFL); return r < 0 ? r : oflags_to_darwin(r); }
    case 4: return fcntl(fd, F_SETFL, oflags_from_darwin((int)arg));
    case 48: return 0;              // F_NOCACHE
    case 51: return fsync(fd);      // F_FULLFSYNC
    case 85: return fsync(fd);      // F_BARRIERFSYNC
    default:
        LOGW("fcntl: unsupported cmd %d", cmd);
        errno = EINVAL;
        return -1;
    }
}

struct darwin_timespec { long tv_sec; long tv_nsec; };
struct darwin_stat {
    int32_t st_dev;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint64_t st_ino;
    uint32_t st_uid;
    uint32_t st_gid;
    int32_t st_rdev;
    int32_t pad0;
    struct darwin_timespec st_atimespec, st_mtimespec, st_ctimespec, st_birthtimespec;
    int64_t st_size;
    int64_t st_blocks;
    int32_t st_blksize;
    uint32_t st_flags;
    uint32_t st_gen;
    int32_t st_lspare;
    int64_t st_qspare[2];
};
_Static_assert(sizeof(struct darwin_stat) == 144, "darwin stat size");

static void stat_to_darwin(const struct stat *s, struct darwin_stat *d) {
    memset(d, 0, sizeof(*d));
    d->st_dev = (int32_t)s->st_dev;
    d->st_mode = (uint16_t)s->st_mode;
    d->st_nlink = (uint16_t)s->st_nlink;
    d->st_ino = s->st_ino;
    d->st_uid = s->st_uid;
    d->st_gid = s->st_gid;
    d->st_rdev = (int32_t)s->st_rdev;
    d->st_atimespec.tv_sec = s->st_atim.tv_sec; d->st_atimespec.tv_nsec = s->st_atim.tv_nsec;
    d->st_mtimespec.tv_sec = s->st_mtim.tv_sec; d->st_mtimespec.tv_nsec = s->st_mtim.tv_nsec;
    d->st_ctimespec.tv_sec = s->st_ctim.tv_sec; d->st_ctimespec.tv_nsec = s->st_ctim.tv_nsec;
    d->st_birthtimespec = d->st_ctimespec;
    d->st_size = s->st_size;
    d->st_blocks = s->st_blocks;
    d->st_blksize = (int32_t)s->st_blksize;
}

int darwin_stat(const char *path, struct darwin_stat *out) {
    struct stat s;
    int r = stat(path, &s);
    if (r == 0) stat_to_darwin(&s, out);
    LOGV("stat(%s) = %d", path, r);
    return r;
}
int darwin_lstat(const char *path, struct darwin_stat *out) {
    struct stat s;
    int r = lstat(path, &s);
    if (r == 0) stat_to_darwin(&s, out);
    return r;
}
int darwin_fstat(int fd, struct darwin_stat *out) {
    struct stat s;
    int r = fstat(fd, &s);
    if (r == 0) stat_to_darwin(&s, out);
    return r;
}

struct darwin_dirent {
    uint64_t d_ino;
    uint64_t d_seekoff;
    uint16_t d_reclen;
    uint16_t d_namlen;
    uint8_t d_type;
    char d_name[1024];
};
typedef struct { DIR *dir; struct darwin_dirent ent; } darwin_DIR;

void *darwin_opendir(const char *path) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    darwin_DIR *w = calloc(1, sizeof(*w));
    w->dir = d;
    return w;
}
struct darwin_dirent *darwin_readdir(void *vd) {
    darwin_DIR *w = vd;
    struct dirent *e = readdir(w->dir);
    if (!e) return NULL;
    w->ent.d_ino = e->d_ino;
    w->ent.d_seekoff = 0;
    w->ent.d_type = e->d_type;  // DT_* values match
    size_t n = strlen(e->d_name);
    if (n > 1023) n = 1023;
    memcpy(w->ent.d_name, e->d_name, n);
    w->ent.d_name[n] = 0;
    w->ent.d_namlen = (uint16_t)n;
    w->ent.d_reclen = (uint16_t)sizeof(w->ent);
    return &w->ent;
}
int darwin_closedir(void *vd) {
    darwin_DIR *w = vd;
    int r = closedir(w->dir);
    free(w);
    return r;
}

FILE *darwin_fopen(const char *path, const char *mode) {
    FILE *f = fopen(path, mode);
    LOGV("fopen(%s, %s) = %p", path, mode, (void *)f);
    return f;
}

int darwin_mkdir(const char *path, int mode) {
    int r = mkdir(path, (mode_t)mode);
    LOGV("mkdir(%s) = %d", path, r);
    return r;
}

// ---------------------------------------------------------------------------
// misc libc
void darwin_assert_rtn(const char *func, const char *file, int line, const char *expr) {
    port_fatal("assertion failed: %s, function %s, file %s:%d", expr, func, file, line);
}

uintptr_t darwin_stack_chk_guard = 0x5a17c0deba5eba11ul;
void darwin_stack_chk_fail(void) { port_fatal("stack smashing detected in game code"); }

void *darwin_memcpy_chk(void *d, const void *s, size_t n, size_t dn) {
    if (n > dn) port_fatal("__memcpy_chk overflow");
    return memcpy(d, s, n);
}
void *darwin_memmove_chk(void *d, const void *s, size_t n, size_t dn) {
    if (n > dn) port_fatal("__memmove_chk overflow");
    return memmove(d, s, n);
}
void *darwin_memset_chk(void *d, int c, size_t n, size_t dn) {
    if (n > dn) port_fatal("__memset_chk overflow");
    return memset(d, c, n);
}
void darwin_memset_pattern16(void *b, const void *pattern, size_t len) {
    uint8_t *p = b;
    while (len >= 16) { memcpy(p, pattern, 16); p += 16; len -= 16; }
    if (len) memcpy(p, pattern, len);
}
void darwin_bzero(void *p, size_t n) { memset(p, 0, n); }
double darwin_exp10(double x) { return pow(10.0, x); }
void darwin_OSMemoryBarrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// Apple Libc rand(): Park-Miller "minimal standard" generator.
static unsigned long g_rand_next = 1;
int darwin_rand(void) {
    long hi, lo, x;
    if (g_rand_next == 0) g_rand_next = 123459876;
    hi = (long)g_rand_next / 127773;
    lo = (long)g_rand_next % 127773;
    x = 16807 * lo - 2836 * hi;
    if (x < 0) x += 0x7fffffff;
    g_rand_next = (unsigned long)x;
    return (int)(x % ((unsigned long)0x7fffffff + 1));
}
void darwin_srand(unsigned seed) { g_rand_next = seed; }

long darwin_sysconf(int name) {
    switch (name) {
    case 29: return sysconf(_SC_PAGESIZE);
    case 57: return sysconf(_SC_NPROCESSORS_CONF);
    case 58: return sysconf(_SC_NPROCESSORS_ONLN);
    case 200: return sysconf(_SC_PHYS_PAGES);
    case 3: return sysconf(_SC_CLK_TCK);
    default: LOGW("sysconf(%d) unsupported", name); errno = EINVAL; return -1;
    }
}

static char g_hw_machine[32] = "iPad8,1";
void darwin_set_hw_machine(const char *m) { strncpy(g_hw_machine, m, sizeof g_hw_machine - 1); }

int darwin_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    const char *sval = NULL;
    long long ival = 0;
    int is_int = 0;
    if (!strcmp(name, "hw.machine") || !strcmp(name, "hw.model")) sval = g_hw_machine;
    else if (!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.activecpu") || !strcmp(name, "hw.logicalcpu")) {
        ival = sysconf(_SC_NPROCESSORS_ONLN); is_int = 4;
    } else if (!strcmp(name, "hw.memsize")) {
        ival = (long long)sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE); is_int = 8;
    } else {
        LOGW("sysctlbyname(%s) unsupported", name);
        errno = ENOENT;
        return -1;
    }
    if (sval) {
        size_t n = strlen(sval) + 1;
        if (!oldp) { *oldlenp = n; return 0; }
        if (*oldlenp < n) { errno = ENOMEM; return -1; }
        memcpy(oldp, sval, n);
        *oldlenp = n;
        return 0;
    }
    if (!oldp) { *oldlenp = (size_t)is_int; return 0; }
    if (is_int == 4) { int v = (int)ival; memcpy(oldp, &v, 4); } else memcpy(oldp, &ival, 8);
    *oldlenp = (size_t)is_int;
    return 0;
}

void darwin_dyld_register_func_for_add_image(void *fn) {
    (void)fn;  // ARCLite hooks; nothing to patch in our runtime.
}

// ---------------------------------------------------------------------------
// ctype (Darwin rune table)
struct darwin_RuneLocale {
    char magic[8];
    char encoding[32];
    void *sgetrune;
    void *sputrune;
    int32_t invalid_rune;
    uint32_t runetype[256];
    int32_t maplower[256];
    int32_t mapupper[256];
    uint8_t rest[256];
};
struct darwin_RuneLocale darwin_DefaultRuneLocale;

#define DCT_A 0x00000100L
#define DCT_C 0x00000200L
#define DCT_D 0x00000400L
#define DCT_G 0x00000800L
#define DCT_L 0x00001000L
#define DCT_P 0x00002000L
#define DCT_S 0x00004000L
#define DCT_U 0x00008000L
#define DCT_X 0x00010000L
#define DCT_B 0x00020000L
#define DCT_R 0x00040000L

uint32_t darwin_ctype_mask(int c) {
    uint32_t m = 0;
    if (c < 0 || c > 127) return 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) m |= DCT_A;
    if (c < 32 || c == 127) m |= DCT_C;
    if (c >= '0' && c <= '9') m |= DCT_D | 0;  // digit value bits unused
    if (c > 32 && c < 127) m |= DCT_G;
    if (c >= 'a' && c <= 'z') m |= DCT_L;
    if (c > 32 && c < 127 && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) m |= DCT_P;
    if (c == ' ' || (c >= 9 && c <= 13)) m |= DCT_S;
    if (c >= 'A' && c <= 'Z') m |= DCT_U;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) m |= DCT_X;
    if (c == ' ' || c == '\t') m |= DCT_B;
    if (c >= 32 && c < 127) m |= DCT_R;
    return m;
}

static void init_rune_locale(void) {
    struct darwin_RuneLocale *r = &darwin_DefaultRuneLocale;
    memcpy(r->magic, "RuneMagA", 8);
    strcpy(r->encoding, "NONE");
    r->invalid_rune = 0xfffd;
    for (int c = 0; c < 256; c++) {
        r->runetype[c] = darwin_ctype_mask(c);
        r->maplower[c] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        r->mapupper[c] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
}
int darwin___tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int darwin___toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
uint32_t darwin___maskrune(int c, unsigned long f) { return darwin_ctype_mask(c) & (uint32_t)f; }

// ---------------------------------------------------------------------------
// pthread: Darwin objects are larger than the host's; we keep a pointer to a
// heap-allocated host object inside them and lazily handle static initializers.
#define SIG_MUTEX_INIT 0x32AAABA7L
#define SIG_MUTEX_ERRCHK_INIT 0x32AAABA1L
#define SIG_MUTEX_RECURSIVE_INIT 0x32AAABA2L
#define SIG_MUTEX_FIRSTFIT_INIT 0x32AAABA3L
#define SIG_MUTEX_LIVE 0x4D555458L  // ours
#define SIG_COND_INIT 0x3CB0B1BBL
#define SIG_COND_LIVE 0x434F4E44L
#define SIG_ONCE_INIT 0x30B1BCBAL
#define SIG_ONCE_DONE 0x444F4E45L
#define SIG_ATTR 0x54484154L
#define SIG_MATTR 0x4D415454L

typedef struct { long sig; pthread_mutex_t *m; char pad[48]; } d_mutex_t;
typedef struct { long sig; int type; int pad; } d_mutexattr_t;
typedef struct { long sig; pthread_cond_t *c; char pad[32]; } d_cond_t;
typedef struct { long sig; long done; } d_once_t;
typedef struct { long sig; size_t stacksize; int detached; int policy; int prio; } d_attr_t;
_Static_assert(sizeof(d_mutex_t) == 64, "mutex");
_Static_assert(sizeof(d_cond_t) == 48, "cond");
_Static_assert(sizeof(d_attr_t) <= 64, "attr");

static pthread_mutex_t g_lazy_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *host_mutex(d_mutex_t *dm, int type) {
    pthread_mutex_t *m = malloc(sizeof(*m));
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, type);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    dm->m = m;
    __atomic_store_n(&dm->sig, SIG_MUTEX_LIVE, __ATOMIC_RELEASE);
    return m;
}

static pthread_mutex_t *get_mutex(d_mutex_t *dm) {
    long sig = __atomic_load_n(&dm->sig, __ATOMIC_ACQUIRE);
    if (sig == SIG_MUTEX_LIVE) return dm->m;
    pthread_mutex_lock(&g_lazy_lock);
    pthread_mutex_t *m;
    sig = dm->sig;
    if (sig == SIG_MUTEX_LIVE) m = dm->m;
    else if (sig == SIG_MUTEX_RECURSIVE_INIT) m = host_mutex(dm, PTHREAD_MUTEX_RECURSIVE);
    else if (sig == SIG_MUTEX_ERRCHK_INIT) m = host_mutex(dm, PTHREAD_MUTEX_ERRORCHECK);
    else {
        if (sig != SIG_MUTEX_INIT && sig != SIG_MUTEX_FIRSTFIT_INIT)
            LOGW("pthread: mutex %p with unknown sig %#lx; initializing", (void *)dm, sig);
        m = host_mutex(dm, PTHREAD_MUTEX_NORMAL);
    }
    pthread_mutex_unlock(&g_lazy_lock);
    return m;
}

int darwin_pthread_mutexattr_init(d_mutexattr_t *a) { a->sig = SIG_MATTR; a->type = 0; return 0; }
int darwin_pthread_mutexattr_destroy(d_mutexattr_t *a) { a->sig = 0; return 0; }
int darwin_pthread_mutexattr_settype(d_mutexattr_t *a, int type) {
    if (type < 0 || type > 2) return EINVAL;
    a->type = type;
    return 0;
}
int darwin_pthread_mutex_init(d_mutex_t *dm, const d_mutexattr_t *a) {
    int t = a ? a->type : 0;
    int host_type = t == 2 ? PTHREAD_MUTEX_RECURSIVE : t == 1 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL;
    host_mutex(dm, host_type);
    return 0;
}
int darwin_pthread_mutex_destroy(d_mutex_t *dm) {
    if (dm->sig == SIG_MUTEX_LIVE && dm->m) {
        pthread_mutex_destroy(dm->m);
        free(dm->m);
    }
    dm->m = NULL;
    dm->sig = 0;
    return 0;
}
int darwin_pthread_mutex_lock(d_mutex_t *dm) { return pthread_mutex_lock(get_mutex(dm)); }
int darwin_pthread_mutex_trylock(d_mutex_t *dm) { return pthread_mutex_trylock(get_mutex(dm)); }
int darwin_pthread_mutex_unlock(d_mutex_t *dm) { return pthread_mutex_unlock(get_mutex(dm)); }

static pthread_cond_t *get_cond(d_cond_t *dc) {
    long sig = __atomic_load_n(&dc->sig, __ATOMIC_ACQUIRE);
    if (sig == SIG_COND_LIVE) return dc->c;
    pthread_mutex_lock(&g_lazy_lock);
    if (dc->sig != SIG_COND_LIVE) {
        pthread_cond_t *c = malloc(sizeof(*c));
        pthread_cond_init(c, NULL);
        dc->c = c;
        __atomic_store_n(&dc->sig, SIG_COND_LIVE, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_lazy_lock);
    return dc->c;
}
int darwin_pthread_cond_init(d_cond_t *dc, const void *attr) {
    (void)attr;
    dc->sig = SIG_COND_INIT;
    get_cond(dc);
    return 0;
}
int darwin_pthread_cond_destroy(d_cond_t *dc) {
    if (dc->sig == SIG_COND_LIVE && dc->c) {
        pthread_cond_destroy(dc->c);
        free(dc->c);
    }
    dc->c = NULL;
    dc->sig = 0;
    return 0;
}
int darwin_pthread_cond_signal(d_cond_t *dc) { return pthread_cond_signal(get_cond(dc)); }
int darwin_pthread_cond_broadcast(d_cond_t *dc) { return pthread_cond_broadcast(get_cond(dc)); }
int darwin_pthread_cond_wait(d_cond_t *dc, d_mutex_t *dm) { return pthread_cond_wait(get_cond(dc), get_mutex(dm)); }
int darwin_pthread_cond_timedwait(d_cond_t *dc, d_mutex_t *dm, const struct timespec *abst) {
    int r = pthread_cond_timedwait(get_cond(dc), get_mutex(dm), abst);
    return r == ETIMEDOUT ? 60 : r;
}

int darwin_pthread_once(d_once_t *o, void (*fn)(void)) {
    if (__atomic_load_n(&o->sig, __ATOMIC_ACQUIRE) == SIG_ONCE_DONE) return 0;
    static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&once_lock);
    if (o->sig != SIG_ONCE_DONE) {
        fn();
        __atomic_store_n(&o->sig, SIG_ONCE_DONE, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once_lock);
    return 0;
}

int darwin_pthread_attr_init(d_attr_t *a) { memset(a, 0, sizeof(*a)); a->sig = SIG_ATTR; return 0; }
int darwin_pthread_attr_destroy(d_attr_t *a) { a->sig = 0; return 0; }
int darwin_pthread_attr_setstacksize(d_attr_t *a, size_t s) { a->stacksize = s; return 0; }
int darwin_pthread_attr_setdetachstate(d_attr_t *a, int st) { a->detached = st == 2; return 0; }  // PTHREAD_CREATE_DETACHED=2
int darwin_pthread_attr_setschedpolicy(d_attr_t *a, int p) { a->policy = p; return 0; }
int darwin_pthread_attr_setschedparam(d_attr_t *a, const int *param) { a->prio = param ? *param : 0; return 0; }

typedef struct { void *(*fn)(void *); void *arg; } thread_start;
static void *thread_trampoline(void *p) {
    thread_start ts = *(thread_start *)p;
    free(p);
    return ts.fn(ts.arg);
}
int darwin_pthread_create(pthread_t *th, const d_attr_t *a, void *(*fn)(void *), void *arg) {
    pthread_attr_t ha;
    pthread_attr_init(&ha);
    size_t ss = (a && a->sig == SIG_ATTR && a->stacksize) ? a->stacksize : 0;
    if (ss < 1024 * 1024) ss = 1024 * 1024;  // iOS secondary threads default to 512K; be generous
    pthread_attr_setstacksize(&ha, ss);
    if (a && a->sig == SIG_ATTR && a->detached) pthread_attr_setdetachstate(&ha, PTHREAD_CREATE_DETACHED);
    thread_start *ts = malloc(sizeof(*ts));
    ts->fn = fn;
    ts->arg = arg;
    int r = pthread_create(th, &ha, thread_trampoline, ts);
    pthread_attr_destroy(&ha);
    LOGV("pthread_create(fn=%p) = %d", (void *)fn, r);  // the game starts short-lived threads every frame
    return r;
}
int darwin_pthread_join(pthread_t th, void **ret) { return pthread_join(th, ret); }
pthread_t darwin_pthread_self(void) { return pthread_self(); }
int darwin_pthread_setname_np(const char *name) {
    char buf[16];
    strncpy(buf, name, 15);
    buf[15] = 0;
    return pthread_setname_np(pthread_self(), buf);
}
int darwin_pthread_getschedparam(pthread_t th, int *policy, int *param) {
    (void)th;
    if (policy) *policy = 1;  // SCHED_OTHER on Darwin
    if (param) { param[0] = 31; param[1] = 0; }
    return 0;
}
int darwin_pthread_setschedparam(pthread_t th, int policy, const int *param) {
    (void)th; (void)policy; (void)param;
    return 0;
}
int darwin_sched_get_priority_min(int p) { (void)p; return 15; }
int darwin_sched_get_priority_max(int p) { (void)p; return 47; }

int darwin_pthread_key_create(unsigned long *key, void (*dtor)(void *)) {
    pthread_key_t k;
    int r = pthread_key_create(&k, dtor);
    if (r == 0) *key = (unsigned long)k;
    return r;
}
void *darwin_pthread_getspecific(unsigned long key) { return pthread_getspecific((pthread_key_t)key); }
int darwin_pthread_setspecific(unsigned long key, const void *v) { return pthread_setspecific((pthread_key_t)key, v); }

// ---------------------------------------------------------------------------
// Mach
// FTL_TIMESCALE (harness only): slow the game's clocks to compensate for
// emulation speed when testing.
static double g_timescale = 0;
static double timescale(void) {
    if (g_timescale == 0) {
        const char *e = getenv("FTL_TIMESCALE");
        g_timescale = e ? atof(e) : 1.0;
        if (g_timescale <= 0) g_timescale = 1.0;
    }
    return g_timescale;
}
uint64_t darwin_mach_absolute_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    double s = timescale();
    return s == 1.0 ? ns : (uint64_t)((double)ns * s);
}
int darwin_gettimeofday(struct timeval *tv, void *tz) {
    int r = gettimeofday(tv, tz);
    double s = timescale();
    if (s != 1.0 && tv) {
        static double base = -1;
        double t = (double)tv->tv_sec + tv->tv_usec * 1e-6;
        if (base < 0) base = t;
        t = base + (t - base) * s;
        tv->tv_sec = (time_t)t;
        tv->tv_usec = (suseconds_t)((t - (double)tv->tv_sec) * 1e6);
    }
    return r;
}
int darwin_mach_timebase_info(uint32_t *info) {
    info[0] = 1;
    info[1] = 1;
    return 0;
}
int darwin_mach_wait_until(uint64_t deadline) {
    struct timespec ts = {(time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    return 0;
}
uint32_t darwin_mach_task_self_ = 0x103;
uint32_t darwin_mach_host_self(void) { return 0x203; }
int darwin_host_page_size(uint32_t host, uintptr_t *size) {
    (void)host;
    *size = 16384;
    return 0;
}

static void read_meminfo(uint64_t *total, uint64_t *avail) {
    *total = (uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)sysconf(_SC_PAGESIZE);
    *avail = *total / 2;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned long long kb;
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) *avail = kb * 1024;
    }
    fclose(f);
}

// HOST_VM_INFO (flavor 2): vm_statistics_data_t (15 x natural_t)
int darwin_host_statistics(uint32_t host, int flavor, int32_t *info, uint32_t *count) {
    (void)host;
    if (flavor != 2) { LOGW("host_statistics flavor %d", flavor); return 4; }
    uint64_t total, avail;
    read_meminfo(&total, &avail);
    uint32_t n = count ? *count : 15;
    memset(info, 0, n * 4);
    uint64_t pages_free = avail / 16384, pages_total = total / 16384;
    if (n > 0) info[0] = (int32_t)pages_free;                          // free_count
    if (n > 1) info[1] = (int32_t)((pages_total - pages_free) / 2);    // active_count
    if (n > 2) info[2] = (int32_t)(pages_free / 4);                    // inactive_count
    if (n > 3) info[3] = (int32_t)((pages_total - pages_free) / 4);    // wire_count
    return 0;
}

// task_info: MACH_TASK_BASIC_INFO (20) / TASK_BASIC_INFO_64 (5)
int darwin_task_info(uint32_t task, int flavor, int32_t *info, uint32_t *count) {
    (void)task;
    long rss_pages = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        long sz;
        if (fscanf(f, "%ld %ld", &sz, &rss_pages) != 2) rss_pages = 0;
        fclose(f);
    }
    uint64_t rss = (uint64_t)rss_pages * (uint64_t)sysconf(_SC_PAGESIZE);
    if (flavor == 20) {  // virtual_size(8) resident_size(8) resident_size_max(8) user_time(8) system_time(8) policy(4) suspend_count(4)
        memset(info, 0, (count ? *count : 12) * 4);
        uint64_t vals[3] = {rss * 4, rss, rss};
        memcpy(info, vals, sizeof vals);
        return 0;
    }
    if (flavor == 5 || flavor == 18) {  // TASK_BASIC_INFO_64(_2), pack(4): suspend_count(4) virtual_size(8) resident_size(8) ...
        memset(info, 0, (count ? *count : 10) * 4);
        uint64_t v = rss * 4, r = rss;
        memcpy((uint8_t *)info + 4, &v, 8);
        memcpy((uint8_t *)info + 12, &r, 8);
        return 0;
    }
    LOGW("task_info flavor %d", flavor);
    return 4;
}

// Mach semaphores, identified by small integer "port names".
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    long count;
    int used;
} msem;
#define MAX_SEMS 256
static msem g_sems[MAX_SEMS];
static pthread_mutex_t g_sem_lock = PTHREAD_MUTEX_INITIALIZER;

int darwin_semaphore_create(uint32_t task, uint32_t *sem, int policy, int value) {
    (void)task; (void)policy;
    pthread_mutex_lock(&g_sem_lock);
    for (int i = 1; i < MAX_SEMS; i++) {
        if (!g_sems[i].used) {
            g_sems[i].used = 1;
            pthread_mutex_init(&g_sems[i].m, NULL);
            pthread_condattr_t ca;
            pthread_condattr_init(&ca);
            pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
            pthread_cond_init(&g_sems[i].c, &ca);
            pthread_condattr_destroy(&ca);
            g_sems[i].count = value;
            *sem = (uint32_t)(0x1000 + i);
            pthread_mutex_unlock(&g_sem_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_sem_lock);
    return 6;  // KERN_RESOURCE_SHORTAGE
}
static msem *sem_get(uint32_t s) {
    uint32_t i = s - 0x1000;
    if (i == 0 || i >= MAX_SEMS || !g_sems[i].used) return NULL;
    return &g_sems[i];
}
int darwin_semaphore_destroy(uint32_t task, uint32_t s) {
    (void)task;
    msem *m = sem_get(s);
    if (!m) return 15;
    m->used = 0;
    return 0;
}
int darwin_semaphore_signal(uint32_t s) {
    msem *m = sem_get(s);
    if (!m) return 15;
    pthread_mutex_lock(&m->m);
    m->count++;
    pthread_cond_signal(&m->c);
    pthread_mutex_unlock(&m->m);
    return 0;
}
int darwin_semaphore_signal_all(uint32_t s) {
    msem *m = sem_get(s);
    if (!m) return 15;
    pthread_mutex_lock(&m->m);
    if (m->count < 0) m->count = 0;
    pthread_cond_broadcast(&m->c);
    pthread_mutex_unlock(&m->m);
    return 0;
}
// Threads parked in a semaphore wait (e.g. the game thread while the app is
// suspended) run the platform's wait hook every 50 ms, so the GL thread can
// still give up its window surface when Android destroys it.
static void (*g_wait_hook)(void);
void darwin_set_wait_hook(void (*fn)(void)) { g_wait_hook = fn; }
static int sem_wait_slice(msem *m, const struct timespec *deadline) {
    struct timespec now, t;
    clock_gettime(CLOCK_MONOTONIC, &now);
    t = now;
    t.tv_nsec += 50000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    bool last = deadline && (deadline->tv_sec < t.tv_sec || (deadline->tv_sec == t.tv_sec && deadline->tv_nsec <= t.tv_nsec));
    int r = pthread_cond_timedwait(&m->c, &m->m, last ? deadline : &t);
    if (r == ETIMEDOUT && !last) {
        pthread_mutex_unlock(&m->m);
        g_wait_hook();
        pthread_mutex_lock(&m->m);
        return 0;
    }
    return r;
}
int darwin_semaphore_wait(uint32_t s) {
    msem *m = sem_get(s);
    if (!m) return 15;
    pthread_mutex_lock(&m->m);
    while (m->count <= 0) {
        if (g_wait_hook) sem_wait_slice(m, NULL);
        else pthread_cond_wait(&m->c, &m->m);
    }
    m->count--;
    pthread_mutex_unlock(&m->m);
    return 0;
}
// mach_timespec_t {unsigned tv_sec; int tv_nsec} is passed by value in x1.
int darwin_semaphore_timedwait(uint32_t s, uint64_t ts_packed) {
    msem *m = sem_get(s);
    if (!m) return 15;
    uint32_t sec = (uint32_t)ts_packed;
    int32_t nsec = (int32_t)(ts_packed >> 32);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec dl = {now.tv_sec + sec, now.tv_nsec + nsec};
    while (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&m->m);
    int rc = 0;
    while (m->count <= 0) {
        int r = g_wait_hook ? sem_wait_slice(m, &dl) : pthread_cond_timedwait(&m->c, &m->m, &dl);
        if (r == ETIMEDOUT) { rc = 49; break; }  // KERN_OPERATION_TIMED_OUT
    }
    if (!rc) m->count--;
    pthread_mutex_unlock(&m->m);
    return rc;
}

// ---------------------------------------------------------------------------
// <search.h>-style hash tables used by ARCLite (hash_create/hash_search).
typedef struct dhash_ent { char *key; void *data; struct dhash_ent *next; } dhash_ent;
typedef struct { dhash_ent *b[257]; } dhash;
void *darwin_hash_create(unsigned n) {
    (void)n;
    return calloc(1, sizeof(dhash));
}
void *darwin_hash_search(void *t, const char *key, void *datum, void (*replace)(void *)) {
    dhash *h = t;
    uint32_t v = 0;
    for (const char *p = key; *p; p++) v = v * 31 + (uint8_t)*p;
    dhash_ent **slot = &h->b[v % 257];
    for (dhash_ent *e = *slot; e; e = e->next) {
        if (!strcmp(e->key, key)) {
            if (datum) {
                if (replace) replace(e->data);
                e->data = datum;
            }
            return e;
        }
    }
    if (!datum) return NULL;
    dhash_ent *e = malloc(sizeof(*e));
    e->key = strdup(key);
    e->data = datum;
    e->next = *slot;
    *slot = e;
    return e;
}

// ---------------------------------------------------------------------------
void darwin_libsystem_init(void) {
    init_rune_locale();
    darwin_stack_chk_guard = 0x5a17c0deba5eba11ul ^ (uintptr_t)darwin_mach_absolute_time();
}

void darwin_exit(int code) {
    LOGI("game called exit(%d)", code);
    port_on_game_exit(code);
    exit(code);
}
