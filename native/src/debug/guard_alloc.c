// Debug allocator for hunting heap corruption in the game (FTL_GUARD=1).
// Every allocation ends exactly at a PROT_NONE guard page, and freed blocks
// become inaccessible, so overflows and use-after-free fault on the guilty
// instruction. Pointers not owned by us are forwarded to the host allocator.
#define _GNU_SOURCE
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <stdio.h>
#include "port_log.h"

#define REGION_SIZE (32ull << 30)
#define PAGE 4096ul

static uint8_t *g_region, *g_next;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uintptr_t ptr; size_t size; uintptr_t base; size_t span; } gent;
static gent *g_tab;
static size_t g_tabmask;

static void guard_init(void) {
    g_region = mmap(NULL, REGION_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_region == MAP_FAILED) { LOGE("guard: reserve failed"); abort(); }
    g_next = g_region;
    g_tabmask = (1u << 22) - 1;
    extern void *__libc_calloc(size_t, size_t);
    g_tab = __libc_calloc(g_tabmask + 1, sizeof(gent));
    fprintf(stderr, "guard allocator active: region %p\n", (void *)g_region);
}
static int owned(const void *p) { return g_region && (uint8_t *)p >= g_region && (uint8_t *)p < g_region + REGION_SIZE; }
static gent *find(uintptr_t p, int insert) {
    size_t i = (p >> 4) & g_tabmask;
    while (g_tab[i].ptr && g_tab[i].ptr != p) i = (i + 1) & g_tabmask;
    if (!g_tab[i].ptr && !insert) return NULL;
    return &g_tab[i];
}

void *guard_malloc(size_t n) {
    pthread_mutex_lock(&g_lock);
    if (!g_region) guard_init();
    size_t sz = (n + 15) & ~15ul;
    if (!sz) sz = 16;
    size_t pages = (sz + PAGE - 1) / PAGE;
    uint8_t *base = g_next;
    g_next += (pages + 1) * PAGE;
    if (mprotect(base, pages * PAGE, PROT_READ | PROT_WRITE) != 0) { LOGE("guard: mprotect failed"); abort(); }
    uint8_t *p = base + pages * PAGE - sz;
    gent *e = find((uintptr_t)p, 1);
    *e = (gent){(uintptr_t)p, n, (uintptr_t)base, pages * PAGE};
    pthread_mutex_unlock(&g_lock);
    return p;
}
void guard_free(void *p) {
    if (!p) return;
    if (!owned(p)) { extern void __libc_free(void *); __libc_free(p); return; }
    pthread_mutex_lock(&g_lock);
    gent *e = find((uintptr_t)p, 0);
    if (!e) { pthread_mutex_unlock(&g_lock); LOGE("guard: free of unknown/double-freed %p", p); abort(); }
    mprotect((void *)e->base, e->span, PROT_NONE);
    // keep the entry (size 0 marks freed) so a double free is reported
    e->size = (size_t)-1;
    pthread_mutex_unlock(&g_lock);
}
void *guard_calloc(size_t a, size_t b) {
    void *p = guard_malloc(a * b);
    memset(p, 0, a * b);
    return p;
}
void *guard_realloc(void *p, size_t n) {
    if (!p) return guard_malloc(n);
    size_t old;
    if (owned(p)) {
        pthread_mutex_lock(&g_lock);
        gent *e = find((uintptr_t)p, 0);
        old = e ? e->size : 0;
        pthread_mutex_unlock(&g_lock);
        if (old == (size_t)-1) { LOGE("guard: realloc of freed %p", p); abort(); }
    } else {
        old = malloc_usable_size(p);
    }
    void *q = guard_malloc(n);
    memcpy(q, p, old < n ? old : n);
    guard_free(p);
    return q;
}

// Process-wide free/realloc interposers (harness only) so pointers from the
// guard region are handled no matter who frees them (game, libc++, us).
extern void __libc_free(void *);
extern void *__libc_realloc(void *, size_t);
__attribute__((visibility("default"))) void free(void *p) {
    if (owned(p)) guard_free(p);
    else __libc_free(p);
}
__attribute__((visibility("default"))) void *realloc(void *p, size_t n) {
    if (owned(p)) return guard_realloc(p, n);
    return __libc_realloc(p, n);
}

// Guard every allocation whose caller is the game image or our executable
// (this includes our libc++), leaving system libraries (Mesa/LLVM) on glibc.
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern char __executable_start[], etext[];
extern struct mo_image *port_image(void);
static int g_guard_all = -1;
static int caller_guarded(void *ra) {
    if (g_guard_all < 0) g_guard_all = getenv("FTL_GUARD") != NULL;
    if (!g_guard_all) return 0;
    if ((char *)ra >= __executable_start && (char *)ra < etext) return 1;
    return 0;
}
__attribute__((visibility("default"))) void *malloc(size_t n) {
    if (caller_guarded(__builtin_return_address(0))) return guard_malloc(n);
    return __libc_malloc(n);
}
__attribute__((visibility("default"))) void *calloc(size_t a, size_t b) {
    if (caller_guarded(__builtin_return_address(0))) return guard_calloc(a, b);
    return __libc_calloc(a, b);
}
size_t guard_usable_size(void *p) {
    if (!owned(p)) return malloc_usable_size(p);
    pthread_mutex_lock(&g_lock);
    gent *e = find((uintptr_t)p, 0);
    size_t s = e ? e->size : 0;
    pthread_mutex_unlock(&g_lock);
    return s;
}
