// Core of the Apple-ABI Objective-C runtime: selectors, classes, method
// lookup + caches, +initialize, refcounting, autorelease pools, and
// registration of classes found in a loaded Mach-O image.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../loader/macho.h"
#include "objc.h"
#include "port_log.h"

static pthread_mutex_t g_rt_lock;
static pthread_cond_t g_init_cond = PTHREAD_COND_INITIALIZER;
#define LOCK() pthread_mutex_lock(&g_rt_lock)
#define UNLOCK() pthread_mutex_unlock(&g_rt_lock)

// ---- selectors --------------------------------------------------------------------
static struct {
    const char **slots;
    uint32_t mask, count;
} g_sels;

static uint64_t str_hash(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; }
    return h;
}

static void sel_grow(void) {
    uint32_t ncap = g_sels.mask ? (g_sels.mask + 1) * 2 : 4096;
    const char **ns = calloc(ncap, sizeof(*ns));
    for (uint32_t i = 0; g_sels.mask && i <= g_sels.mask; i++) {
        const char *s = g_sels.slots[i];
        if (!s) continue;
        uint32_t j = (uint32_t)str_hash(s) & (ncap - 1);
        while (ns[j]) j = (j + 1) & (ncap - 1);
        ns[j] = s;
    }
    free(g_sels.slots);
    g_sels.slots = ns;
    g_sels.mask = ncap - 1;
}

// `persistent` names are stored without copying (string literals / image strings).
static SEL sel_intern(const char *name, int persistent) {
    if (!name) return NULL;
    if (!g_sels.mask || (g_sels.count + 1) * 4 > (g_sels.mask + 1) * 3) sel_grow();
    uint32_t j = (uint32_t)str_hash(name) & g_sels.mask;
    while (g_sels.slots[j]) {
        if (!strcmp(g_sels.slots[j], name)) return g_sels.slots[j];
        j = (j + 1) & g_sels.mask;
    }
    const char *s = persistent ? name : strdup(name);
    g_sels.slots[j] = s;
    g_sels.count++;
    return s;
}

SEL sel_registerName(const char *name) {
    LOCK();
    SEL s = sel_intern(name, 0);
    UNLOCK();
    return s;
}
SEL sel_getUid(const char *name) { return sel_registerName(name); }
const char *sel_getName(SEL sel) { return sel ? sel : "<null selector>"; }
static SEL sel_persistent(const char *name) {
    LOCK();
    SEL s = sel_intern(name, 1);
    UNLOCK();
    return s;
}

// ---- class table -----------------------------------------------------------------
static struct {
    Class *slots;
    uint32_t mask, count;
} g_classes;
static Class *g_all_classes;  // list (for cache flushes)
static size_t g_nall, g_capall;

static void class_table_insert(Class cls, const char *name) {
    if (!g_classes.mask || (g_classes.count + 1) * 4 > (g_classes.mask + 1) * 3) {
        uint32_t ncap = g_classes.mask ? (g_classes.mask + 1) * 2 : 512;
        Class *ns = calloc(ncap, sizeof(*ns));
        for (uint32_t i = 0; g_classes.mask && i <= g_classes.mask; i++) {
            Class c = g_classes.slots[i];
            if (!c) continue;
            uint32_t j = (uint32_t)str_hash(c->rw->name) & (ncap - 1);
            while (ns[j]) j = (j + 1) & (ncap - 1);
            ns[j] = c;
        }
        free(g_classes.slots);
        g_classes.slots = ns;
        g_classes.mask = ncap - 1;
    }
    uint32_t j = (uint32_t)str_hash(name) & g_classes.mask;
    while (g_classes.slots[j]) {
        if (!strcmp(g_classes.slots[j]->rw->name, name)) {
            LOGW("objc: duplicate class %s (keeping first)", name);
            return;
        }
        j = (j + 1) & g_classes.mask;
    }
    g_classes.slots[j] = cls;
    g_classes.count++;
}

static Class class_table_get(const char *name) {
    if (!g_classes.mask || !name) return Nil;
    uint32_t j = (uint32_t)str_hash(name) & g_classes.mask;
    while (g_classes.slots[j]) {
        if (!strcmp(g_classes.slots[j]->rw->name, name)) return g_classes.slots[j];
        j = (j + 1) & g_classes.mask;
    }
    return Nil;
}

static void all_classes_add(Class c) {
    if (g_nall == g_capall) {
        g_capall = g_capall ? g_capall * 2 : 256;
        g_all_classes = realloc(g_all_classes, g_capall * sizeof(Class));
    }
    g_all_classes[g_nall++] = c;
}

// ---- method tables ------------------------------------------------------------------
static inline uint32_t sel_hash(SEL s) {
    uintptr_t v = (uintptr_t)s;
    return (uint32_t)(v ^ (v >> 4));
}

static IMP mtable_get(struct objc_mtable *t, SEL sel) {
    if (!t->mask) return NULL;
    uint32_t j = sel_hash(sel) & t->mask;
    while (t->slots[j].sel) {
        if (t->slots[j].sel == sel) return t->slots[j].imp;
        j = (j + 1) & t->mask;
    }
    return NULL;
}

static IMP mtable_put(struct objc_mtable *t, SEL sel, IMP imp, int replace) {
    if (!t->mask || (t->count + 1) * 4 > (t->mask + 1) * 3) {
        uint32_t ncap = t->mask ? (t->mask + 1) * 2 : 16;
        __typeof__(t->slots) ns = calloc(ncap, sizeof(*ns));
        for (uint32_t i = 0; t->mask && i <= t->mask; i++) {
            if (!t->slots[i].sel) continue;
            uint32_t j = sel_hash(t->slots[i].sel) & (ncap - 1);
            while (ns[j].sel) j = (j + 1) & (ncap - 1);
            ns[j] = t->slots[i];
        }
        free(t->slots);
        t->slots = ns;
        t->mask = ncap - 1;
    }
    uint32_t j = sel_hash(sel) & t->mask;
    while (t->slots[j].sel) {
        if (t->slots[j].sel == sel) {
            IMP old = t->slots[j].imp;
            if (replace) t->slots[j].imp = imp;
            return old;
        }
        j = (j + 1) & t->mask;
    }
    t->slots[j].sel = sel;
    t->slots[j].imp = imp;
    t->count++;
    return NULL;
}

// ---- method caches (read lock-free by objc_msgSend) -----------------------------------
struct objc_cache {
    uint32_t mask;
    uint32_t occupied;
    struct {
        _Atomic(SEL) sel;
        _Atomic(IMP) imp;
    } buckets[];
};
static struct objc_cache g_empty_cache_storage = {0, 0};
// The empty cache needs one readable (zero) bucket after the header.
static struct {
    struct objc_cache c;
    uint64_t b[2];
} g_empty_cache = {{0, 0}, {0, 0}};
uint64_t objc_empty_cache_sym[4];
uint64_t objc_empty_vtable_sym[4];

static struct objc_cache *cache_new(uint32_t cap) {
    struct objc_cache *c = calloc(1, sizeof(struct objc_cache) + cap * 16);
    c->mask = cap - 1;
    return c;
}

static void cache_insert(Class cls, SEL sel, IMP imp) {
    struct objc_cache *c = cls->cache;
    if (c == &g_empty_cache.c || c == &g_empty_cache_storage || !c || (c->occupied + 1) * 4 > (c->mask + 1) * 3) {
        uint32_t ncap = (c && c != &g_empty_cache.c && c != &g_empty_cache_storage) ? (c->mask + 1) * 2 : 8;
        struct objc_cache *n = cache_new(ncap);
        if (c && c != &g_empty_cache.c && c != &g_empty_cache_storage) {
            for (uint32_t i = 0; i <= c->mask; i++) {
                SEL s = atomic_load_explicit(&c->buckets[i].sel, memory_order_relaxed);
                IMP m = atomic_load_explicit(&c->buckets[i].imp, memory_order_relaxed);
                if (!s || !m) continue;
                uint32_t j = sel_hash(s) & n->mask;
                while (n->buckets[j].sel) j = (j + 1) & n->mask;
                n->buckets[j].imp = m;
                n->buckets[j].sel = s;
                n->occupied++;
            }
        }
        // Old caches are leaked on purpose: other threads may still be probing them.
        atomic_thread_fence(memory_order_release);
        cls->cache = n;
        c = n;
    }
    uint32_t j = sel_hash(sel) & c->mask;
    for (;;) {
        SEL s = atomic_load_explicit(&c->buckets[j].sel, memory_order_relaxed);
        if (s == sel) return;
        if (!s) break;
        j = (j + 1) & c->mask;
    }
    atomic_store_explicit(&c->buckets[j].imp, imp, memory_order_relaxed);
    atomic_store_explicit(&c->buckets[j].sel, sel, memory_order_release);
    c->occupied++;
}

static void flush_all_caches(void) {
    for (size_t i = 0; i < g_nall; i++) {
        g_all_classes[i]->cache = &g_empty_cache.c;
        if (g_all_classes[i]->isa) g_all_classes[i]->isa->cache = &g_empty_cache.c;
    }
}

// ---- class realization -------------------------------------------------------------
static inline const struct objc_class_ro *class_ro(Class c) { return (const struct objc_class_ro *)(c->bits & ~(uintptr_t)7); }
static inline int is_meta(Class c) { return c->rw && (c->rw->flags & RW_META); }

static void add_method_list(struct objc_class_rw *rw, const struct objc_method_list *ml) {
    if (!ml) return;
    uint32_t entsize = ml->entsize_flags & ~3u & 0xffff;
    if (ml->entsize_flags & 0x80000000u) {
        LOGE("objc: relative method lists unsupported (%s)", rw->name);
        return;
    }
    for (uint32_t i = 0; i < ml->count; i++) {
        const struct objc_method_t *m = (const struct objc_method_t *)((const uint8_t *)ml->first + i * entsize);
        SEL s = sel_intern(m->name, 1);
        mtable_put(&rw->methods, s, m->imp, 1);
        if (!strcmp(s, ".cxx_construct")) rw->flags |= RW_HAS_CXX_CTOR;
        if (!strcmp(s, ".cxx_destruct")) rw->flags |= RW_HAS_CXX_DTOR;
    }
}

static int realize_image_class(Class cls);

static void add_protocols(struct objc_class_rw *rw, const struct objc_protocol_list *pl) {
    if (!pl) return;
    for (uintptr_t i = 0; i < pl->count; i++) {
        rw->protocols = realloc(rw->protocols, sizeof(void *) * (size_t)(rw->nprotocols + 1));
        rw->protocols[rw->nprotocols++] = pl->list[i];
    }
}

// Realize a class_t that lives in a loaded image (rw slot holds _objc_empty_vtable).
static int realize_image_class(Class cls) {
    if (cls->rw && (uintptr_t)cls->rw != (uintptr_t)objc_empty_vtable_sym && (cls->rw->flags & RW_REALIZED)) return 0;
    const struct objc_class_ro *ro = class_ro(cls);
    Class meta = cls->isa;
    // Superclass first (may itself be an image class).
    Class sup = cls->superclass;
    if (sup && (!sup->rw || (uintptr_t)sup->rw == (uintptr_t)objc_empty_vtable_sym)) realize_image_class(sup);

    struct objc_class_rw *rw = calloc(1, sizeof(*rw));
    rw->flags = RW_REALIZED;
    rw->name = ro->name;
    rw->cls = cls;
    rw->ro = ro;
    rw->instance_size = ro->instance_size;

    // Non-fragile ivars: slide if our superclass is bigger than the compiler assumed.
    if (sup && sup->rw) {
        uint32_t super_size = sup->rw->instance_size;
        if (super_size > ro->instance_start) {
            uint32_t diff = super_size - ro->instance_start;
            uint32_t max_align = 8;
            if (ro->ivars) {
                for (uint32_t i = 0; i < ro->ivars->count; i++) {
                    const struct objc_ivar_t *iv =
                        (const struct objc_ivar_t *)((const uint8_t *)ro->ivars->first + i * ro->ivars->entsize);
                    uint32_t al = iv->alignment_raw == 0xffffffffu ? 8 : (1u << iv->alignment_raw);
                    if (al > max_align) max_align = al;
                }
            }
            diff = (diff + max_align - 1) & ~(max_align - 1);
            if (ro->ivars) {
                for (uint32_t i = 0; i < ro->ivars->count; i++) {
                    const struct objc_ivar_t *iv =
                        (const struct objc_ivar_t *)((const uint8_t *)ro->ivars->first + i * ro->ivars->entsize);
                    if (iv->offset) *iv->offset += (int32_t)diff;
                }
            }
            rw->instance_size = ro->instance_size + diff;
            LOGV("objc: slid ivars of %s by %u (super %u, start %u)", ro->name, diff, super_size, ro->instance_start);
        }
    }
    if (ro->ivars) {
        rw->nivars = (int)ro->ivars->count;
        rw->ivars = calloc((size_t)rw->nivars, sizeof(*rw->ivars));
        for (uint32_t i = 0; i < ro->ivars->count; i++) {
            const struct objc_ivar_t *iv =
                (const struct objc_ivar_t *)((const uint8_t *)ro->ivars->first + i * ro->ivars->entsize);
            rw->ivars[i] = (struct objc_ivar_info){iv->name, iv->type, iv->offset, iv->size};
        }
    }
    add_method_list(rw, ro->base_methods);
    add_protocols(rw, ro->base_protocols);
    cls->rw = rw;
    cls->cache = &g_empty_cache.c;

    // Metaclass.
    const struct objc_class_ro *mro = class_ro(meta);
    struct objc_class_rw *mrw = calloc(1, sizeof(*mrw));
    mrw->flags = RW_REALIZED | RW_META;
    mrw->name = ro->name;
    mrw->cls = meta;
    mrw->nonmeta = cls;
    mrw->ro = mro;
    mrw->instance_size = mro->instance_size;
    add_method_list(mrw, mro->base_methods);
    meta->rw = mrw;
    meta->cache = &g_empty_cache.c;
    if (sup && sup->rw) rw->flags |= sup->rw->flags & (RW_NO_RC);

    class_table_insert(cls, ro->name);
    all_classes_add(cls);
    LOGV("objc: realized image class %s : %s (size %u)", ro->name, sup ? sup->rw->name : "-", rw->instance_size);
    return 0;
}

// ---- class definition helpers (framework classes written in C) ----------------------------
Class rt_metaclass(Class cls) { return cls->isa; }

static Class g_root_meta;  // NSObject's metaclass

Class rt_define_class(const char *name, Class super, size_t instance_size) {
    Class cls = calloc(1, sizeof(struct objc_class));
    Class meta = calloc(1, sizeof(struct objc_class));
    struct objc_class_rw *rw = calloc(1, sizeof(*rw));
    struct objc_class_rw *mrw = calloc(1, sizeof(*mrw));
    struct objc_class_ro *ro = calloc(1, sizeof(*ro));
    struct objc_class_ro *mro = calloc(1, sizeof(*mro));
    ro->name = mro->name = name;
    if (super && instance_size < super->rw->instance_size) instance_size = super->rw->instance_size;
    ro->instance_size = ro->instance_start = (uint32_t)instance_size;
    mro->flags = RO_META;
    rw->flags = RW_REALIZED;
    rw->name = name;
    rw->cls = cls;
    rw->ro = ro;
    rw->instance_size = (uint32_t)instance_size;
    mrw->flags = RW_REALIZED | RW_META;
    mrw->name = name;
    mrw->cls = meta;
    mrw->nonmeta = cls;
    mrw->ro = mro;
    mrw->instance_size = sizeof(struct objc_class);
    cls->rw = rw;
    cls->bits = (uintptr_t)ro;
    cls->cache = &g_empty_cache.c;
    cls->isa = meta;
    cls->superclass = super;
    meta->rw = mrw;
    meta->bits = (uintptr_t)mro;
    meta->cache = &g_empty_cache.c;
    if (super) {
        meta->superclass = super->isa;
        meta->isa = g_root_meta ? g_root_meta : meta;
        rw->flags |= super->rw->flags & RW_NO_RC;
    } else {
        // root class: metaclass's superclass is the class itself, isa is itself
        ro->flags |= RO_ROOT;
        meta->superclass = cls;
        meta->isa = meta;
        if (!g_root_meta) g_root_meta = meta;
    }
    LOCK();
    class_table_insert(cls, name);
    all_classes_add(cls);
    UNLOCK();
    return cls;
}

void rt_method(Class cls, const char *sel, void *imp) {
    LOCK();
    mtable_put(&cls->rw->methods, sel_intern(sel, 1), (IMP)imp, 1);
    flush_all_caches();
    UNLOCK();
}
void rt_cmethod(Class cls, const char *sel, void *imp) { rt_method(cls->isa, sel, imp); }
void rt_set_flags(Class cls, uint32_t flags) { cls->rw->flags |= flags; }

// ---- lookup / +initialize -----------------------------------------------------------
extern void objc_nil_imp(void);

static IMP lookup_imp_locked(Class cls, SEL sel) {
    for (Class c = cls; c; c = c->superclass) {
        if (!c->rw) break;
        IMP imp = mtable_get(&c->rw->methods, sel);
        if (imp) return imp;
    }
    return NULL;
}

static void initialize_class(Class cls);

static void initialize_class(Class cls) {
    if (!cls || !cls->rw) return;
    if (cls->superclass && cls->superclass->rw && !(cls->superclass->rw->flags & RW_INITIALIZED))
        initialize_class(cls->superclass);
    LOCK();
    for (;;) {
        uint32_t f = cls->rw->flags;
        if (f & RW_INITIALIZED) { UNLOCK(); return; }
        if (f & RW_INITIALIZING) {
            if (pthread_equal(cls->rw->init_thread, pthread_self())) { UNLOCK(); return; }
            pthread_cond_wait(&g_init_cond, &g_rt_lock);
            continue;
        }
        break;
    }
    cls->rw->flags |= RW_INITIALIZING;
    cls->rw->init_thread = pthread_self();
    UNLOCK();
    static SEL s_init;
    if (!s_init) s_init = sel_persistent("initialize");
    IMP imp;
    LOCK();
    imp = lookup_imp_locked(cls->isa, s_init);
    UNLOCK();
    if (imp) ((void (*)(Class, SEL))imp)(cls, s_init);
    LOCK();
    cls->rw->flags = (cls->rw->flags & ~RW_INITIALIZING) | RW_INITIALIZED;
    pthread_cond_broadcast(&g_init_cond);
    UNLOCK();
}

typedef struct unrec_entry {
    Class cls;
    SEL sel;
    struct unrec_entry *next;
} unrec_entry;
static unrec_entry *g_unrec;

void rt_log_unrecognized(id self, SEL sel, Class cls) {
    for (unrec_entry *e = g_unrec; e; e = e->next)
        if (e->cls == cls && e->sel == sel) return;
    unrec_entry *e = malloc(sizeof(*e));
    e->cls = cls;
    e->sel = sel;
    e->next = g_unrec;
    g_unrec = e;
    LOGE("objc: unrecognized selector %c[%s %s] (receiver %p)", is_meta(cls) ? '+' : '-',
         cls && cls->rw ? cls->rw->name : "?", sel_getName(sel), (void *)self);
}

IMP objc_msg_lookup_slow(id self, SEL sel, Class cls) {
    if (!cls->rw) {
        LOGE("objc: message %s to unrealized class %p (receiver %p)", sel_getName(sel), (void *)cls, (void *)self);
        return (IMP)objc_nil_imp;
    }
    Class nonmeta = is_meta(cls) ? cls->rw->nonmeta : cls;
    if (nonmeta && nonmeta->rw && !(nonmeta->rw->flags & RW_INITIALIZED)) initialize_class(nonmeta);
    LOCK();
    IMP imp = lookup_imp_locked(cls, sel);
    if (!imp) {
        rt_log_unrecognized(self, sel, cls);
        imp = (IMP)objc_nil_imp;
    }
    if (!nonmeta || (nonmeta->rw->flags & RW_INITIALIZED)) cache_insert(cls, sel, imp);
    UNLOCK();
    return imp;
}

// ---- objects & refcounting ------------------------------------------------------------
// Every refcounted object has a 16-byte header before it: [extra_rc][flags].
typedef struct {
    _Atomic long extra_rc;
    long magic;
} objhdr;
#define OBJ_MAGIC 0x0B1EC7AB1Eul

static inline objhdr *hdr_of(id o) { return (objhdr *)((uint8_t *)o - sizeof(objhdr)); }

id rt_alloc_object(Class cls, size_t size) {
    if (size < sizeof(void *)) size = sizeof(void *);
    objhdr *h = calloc(1, sizeof(objhdr) + size);
    if (!h) return nil;
    h->magic = OBJ_MAGIC;
    id o = (id)(h + 1);
    o->isa = cls;
    return o;
}

static inline int has_rc(id o) {
    Class c = o->isa;
    if (!c || !c->rw || (c->rw->flags & (RW_NO_RC | RW_META))) return 0;
    return hdr_of(o)->magic == (long)OBJ_MAGIC;
}

id rt_retain(id o) {
    if (o && has_rc(o)) atomic_fetch_add_explicit(&hdr_of(o)->extra_rc, 1, memory_order_relaxed);
    return o;
}

bool rt_release(id o) {
    if (!o || !has_rc(o)) return false;
    long prev = atomic_fetch_sub_explicit(&hdr_of(o)->extra_rc, 1, memory_order_acq_rel);
    if (prev == 0) return true;
    if (prev < 0) LOGE("objc: over-release of %p (%s)", (void *)o, o->isa->rw->name);
    return false;
}

long rt_retain_count(id o) {
    if (!o || !has_rc(o)) return 0x7fffffffffffffffL;
    return atomic_load(&hdr_of(o)->extra_rc) + 1;
}

void rt_dealloc_object(id o) {
    if (!o) return;
    // run .cxx_destruct up the hierarchy
    static SEL s_dtor;
    if (!s_dtor) s_dtor = sel_persistent(".cxx_destruct");
    for (Class c = o->isa; c && c->rw; c = c->superclass) {
        if (c->rw->flags & RW_HAS_CXX_DTOR) {
            IMP imp = mtable_get(&c->rw->methods, s_dtor);
            if (imp) ((void (*)(id, SEL))imp)(o, s_dtor);
        }
    }
    if (hdr_of(o)->magic == (long)OBJ_MAGIC) {
        hdr_of(o)->magic = 0;
        o->isa = NULL;
        free(hdr_of(o));
    }
}

id class_createInstance(Class cls, size_t extra) {
    if (!cls) return nil;
    id o = rt_alloc_object(cls, cls->rw->instance_size + extra);
    static SEL s_ctor;
    if (!s_ctor) s_ctor = sel_persistent(".cxx_construct");
    // run .cxx_construct from root to leaf
    Class chain[32];
    int n = 0;
    for (Class c = cls; c && c->rw && n < 32; c = c->superclass) chain[n++] = c;
    for (int i = n - 1; i >= 0; i--) {
        if (chain[i]->rw->flags & RW_HAS_CXX_CTOR) {
            IMP imp = mtable_get(&chain[i]->rw->methods, s_ctor);
            if (imp) ((id(*)(id, SEL))imp)(o, s_ctor);
        }
    }
    return o;
}

id object_dispose(id obj) {
    rt_dealloc_object(obj);
    return nil;
}

void *object_getIndexedIvars(id obj) {
    return obj ? (uint8_t *)obj + obj->isa->rw->instance_size : NULL;
}

// ---- autorelease pools --------------------------------------------------------------
typedef struct arpool {
    id *objs;
    size_t n, cap;
} arpool;
static _Thread_local arpool *t_pools;
static _Thread_local size_t t_npools, t_cappools;

void *objc_autoreleasePoolPush(void) {
    if (t_npools == t_cappools) {
        t_cappools = t_cappools ? t_cappools * 2 : 8;
        t_pools = realloc(t_pools, t_cappools * sizeof(arpool));
    }
    t_pools[t_npools] = (arpool){0};
    return (void *)(uintptr_t)(++t_npools);
}

void objc_autoreleasePoolPop(void *ctx) {
    size_t level = (size_t)(uintptr_t)ctx;
    static SEL s_release;
    if (!s_release) s_release = sel_persistent("release");
    while (t_npools >= level && t_npools > 0) {
        arpool *p = &t_pools[t_npools - 1];
        // objects may autorelease more objects while being released
        while (p->n) {
            id o = p->objs[--p->n];
            ((void (*)(id, SEL))objc_msgSend)(o, s_release);
            p = &t_pools[t_npools - 1];
        }
        free(p->objs);
        t_npools--;
    }
}

id rt_autorelease(id o) {
    if (!o) return o;
    if (!t_npools) {
        static int warned;
        if (!warned++) LOGW("objc: autorelease with no pool (leaking) on thread %lu", (unsigned long)pthread_self());
        return o;
    }
    arpool *p = &t_pools[t_npools - 1];
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->objs = realloc(p->objs, p->cap * sizeof(id));
    }
    p->objs[p->n++] = o;
    return o;
}

// ---- public API ---------------------------------------------------------------------
Class objc_getClass(const char *name) {
    LOCK();
    Class c = class_table_get(name);
    UNLOCK();
    return c;
}
Class objc_lookUpClass(const char *name) { return objc_getClass(name); }
Class objc_getMetaClass(const char *name) {
    Class c = objc_getClass(name);
    return c ? c->isa : Nil;
}
Class objc_getRequiredClass(const char *name) {
    Class c = objc_getClass(name);
    if (!c) port_fatal("objc_getRequiredClass: %s missing", name);
    return c;
}
Class object_getClass(id obj) { return obj ? obj->isa : Nil; }
const char *class_getName(Class cls) { return cls && cls->rw ? cls->rw->name : "nil"; }
Class class_getSuperclass(Class cls) { return cls ? cls->superclass : Nil; }
BOOL class_isMetaClass(Class cls) { return cls && is_meta(cls); }
size_t class_getInstanceSize(Class cls) { return cls && cls->rw ? cls->rw->instance_size : 0; }

BOOL class_addMethod(Class cls, SEL name, IMP imp, const char *types) {
    (void)types;
    if (!cls || !cls->rw) return NO;
    LOCK();
    SEL s = sel_intern(name, 0);
    IMP existing = mtable_get(&cls->rw->methods, s);
    if (!existing) {
        mtable_put(&cls->rw->methods, s, imp, 1);
        flush_all_caches();
    }
    UNLOCK();
    return existing == NULL;
}

IMP class_replaceMethod(Class cls, SEL name, IMP imp, const char *types) {
    (void)types;
    if (!cls || !cls->rw) return NULL;
    LOCK();
    SEL s = sel_intern(name, 0);
    IMP old = mtable_put(&cls->rw->methods, s, imp, 1);
    flush_all_caches();
    UNLOCK();
    return old;
}

IMP class_getMethodImplementation(Class cls, SEL name) {
    if (!cls) return NULL;
    LOCK();
    IMP imp = lookup_imp_locked(cls, name);
    UNLOCK();
    return imp ? imp : (IMP)objc_nil_imp;
}

BOOL class_respondsToSelector(Class cls, SEL sel) {
    if (!cls) return NO;
    LOCK();
    IMP imp = lookup_imp_locked(cls, sel);
    UNLOCK();
    return imp != NULL;
}

BOOL objc_isKindOfClass(id obj, Class cls) {
    if (!obj) return NO;
    for (Class c = obj->isa; c; c = c->superclass)
        if (c == cls) return YES;
    return NO;
}

static SEL s_retain, s_release, s_autorelease, s_alloc;
id objc_retain(id obj) {
    if (!obj) return obj;
    return ((id(*)(id, SEL))objc_msgSend)(obj, s_retain);
}
void objc_release(id obj) {
    if (!obj) return;
    ((void (*)(id, SEL))objc_msgSend)(obj, s_release);
}
id objc_autorelease(id obj) {
    if (!obj) return obj;
    return ((id(*)(id, SEL))objc_msgSend)(obj, s_autorelease);
}
id objc_alloc(Class cls) {
    if (!cls) return nil;
    return ((id(*)(id, SEL))objc_msgSend)((id)cls, s_alloc);
}

void rt_dump_class(Class cls) {
    if (!cls || !cls->rw) return;
    LOGI("class %s size=%u flags=%#x super=%s", cls->rw->name, cls->rw->instance_size, cls->rw->flags,
         cls->superclass && cls->superclass->rw ? cls->superclass->rw->name : "-");
}

// ---- image registration --------------------------------------------------------------
void rt_register_image_objc(void *vimg) {
    mo_image *img = vimg;
    LOCK();
    mo_section *s;
    // 1. selector references -> interned selectors
    if ((s = mo_find_section(img, NULL, "__objc_selrefs"))) {
        const char **refs = (const char **)(uintptr_t)s->addr;
        for (size_t i = 0; i < s->size / 8; i++) refs[i] = sel_intern(refs[i], 1);
    }
    // 2. classes
    if ((s = mo_find_section(img, NULL, "__objc_classlist"))) {
        Class *list = (Class *)(uintptr_t)s->addr;
        for (size_t i = 0; i < s->size / 8; i++) realize_image_class(list[i]);
    }
    // 3. categories
    if ((s = mo_find_section(img, NULL, "__objc_catlist"))) {
        struct objc_category_t **cats = (struct objc_category_t **)(uintptr_t)s->addr;
        for (size_t i = 0; i < s->size / 8; i++) {
            struct objc_category_t *cat = cats[i];
            Class cls = cat->cls;
            if (!cls || !cls->rw) { LOGW("objc: category %s on missing class", cat->name); continue; }
            add_method_list(cls->rw, cat->instance_methods);
            add_method_list(cls->isa->rw, cat->class_methods);
            add_protocols(cls->rw, cat->protocols);
        }
    }
    // 4. protocols get an isa (Protocol class may not exist; leave as-is)
    flush_all_caches();
    UNLOCK();

    // 5. +load for non-lazy classes
    if ((s = mo_find_section(img, NULL, "__objc_nlclslist"))) {
        Class *list = (Class *)(uintptr_t)s->addr;
        SEL s_load = sel_persistent("load");
        for (size_t i = 0; i < s->size / 8; i++) {
            Class c = list[i];
            if (!c->rw || (uintptr_t)c->rw == (uintptr_t)objc_empty_vtable_sym) {
                LOCK();
                realize_image_class(c);
                UNLOCK();
            }
            IMP imp = mtable_get(&c->isa->rw->methods, s_load);
            LOGI("objc: +load %s (%s)", c->rw->name, imp ? "has +load" : "no +load");
            if (imp) ((void (*)(Class, SEL))imp)(c, s_load);
        }
    }
}

void rt_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_rt_lock, &a);
    s_retain = sel_persistent("retain");
    s_release = sel_persistent("release");
    s_autorelease = sel_persistent("autorelease");
    s_alloc = sel_persistent("alloc");
}
