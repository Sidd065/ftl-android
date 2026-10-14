// Less common Objective-C runtime entry points (ARC helpers, properties,
// reflection, blocks). Most are only referenced, rarely called.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../fw/fw.h"
#include "objc.h"
#include "port_log.h"

// ---- Method handles -----------------------------------------------------------
typedef struct method_handle {
    SEL name;
    const char *types;
    IMP imp;
    Class cls;
    struct method_handle *next;
} method_handle;
static method_handle *g_methods;
static pthread_mutex_t g_api_lock = PTHREAD_MUTEX_INITIALIZER;

void *class_getInstanceMethod(Class cls, SEL sel) {
    if (!cls || !sel) return NULL;
    sel = sel_registerName(sel);
    if (!class_respondsToSelector(cls, sel)) return NULL;
    IMP imp = class_getMethodImplementation(cls, sel);
    pthread_mutex_lock(&g_api_lock);
    for (method_handle *m = g_methods; m; m = m->next)
        if (m->cls == cls && m->name == sel) { m->imp = imp; pthread_mutex_unlock(&g_api_lock); return m; }
    method_handle *m = calloc(1, sizeof(*m));
    m->name = sel;
    m->types = "";
    m->imp = imp;
    m->cls = cls;
    m->next = g_methods;
    g_methods = m;
    pthread_mutex_unlock(&g_api_lock);
    return m;
}
void *class_getClassMethod(Class cls, SEL sel) { return cls ? class_getInstanceMethod(cls->isa, sel) : NULL; }
IMP method_setImplementation(void *mh, IMP imp) {
    method_handle *m = mh;
    if (!m) return NULL;
    IMP old = m->imp;
    class_replaceMethod(m->cls, m->name, imp, m->types);
    m->imp = imp;
    return old;
}
IMP method_getImplementation(void *mh) { return mh ? ((method_handle *)mh)->imp : NULL; }
SEL method_getName(void *mh) { return mh ? ((method_handle *)mh)->name : NULL; }

// ---- ivars ------------------------------------------------------------------------
void *class_getInstanceVariable(Class cls, const char *name) {
    for (Class c = cls; c && c->rw; c = c->superclass)
        for (int i = 0; i < c->rw->nivars; i++)
            if (!strcmp(c->rw->ivars[i].name, name)) return &c->rw->ivars[i];
    return NULL;
}
id object_getIvar(id obj, void *ivar) {
    struct objc_ivar_info *iv = ivar;
    if (!obj || !iv || !iv->offset) return nil;
    return *(id *)((uint8_t *)obj + *iv->offset);
}
void object_setIvar(id obj, void *ivar, id val) {
    struct objc_ivar_info *iv = ivar;
    if (!obj || !iv || !iv->offset) return;
    *(id *)((uint8_t *)obj + *iv->offset) = val;
}

// ---- class pairs (dynamic subclassing) -------------------------------------------------
Class objc_allocateClassPair(Class super, const char *name, size_t extra) {
    (void)extra;
    if (objc_getClass(name)) return Nil;
    return rt_define_class(strdup(name), super, super ? class_getInstanceSize(super) : sizeof(void *));
}
void objc_registerClassPair(Class cls) { (void)cls; }
Class objc_initializeClassPair(Class super, const char *name, Class cls, Class meta) {
    (void)cls; (void)meta;
    LOGW("objc_initializeClassPair(%s) unsupported; allocating", name);
    return objc_allocateClassPair(super, name, 0);
}
Class objc_readClassPair(Class cls, const void *info) {
    (void)info;
    LOGW("objc_readClassPair called");
    return cls;
}
id objc_constructInstance(Class cls, void *bytes) {
    if (!cls || !bytes) return nil;
    memset(bytes, 0, class_getInstanceSize(cls));
    ((id)bytes)->isa = cls;
    return (id)bytes;
}
const char **objc_copyClassNamesForImage(const char *image, unsigned *outCount) {
    (void)image;
    if (outCount) *outCount = 0;
    return NULL;
}
BOOL class_addProperty(Class cls, const char *name, const void *attrs, unsigned n) {
    (void)cls; (void)name; (void)attrs; (void)n;
    return YES;
}
BOOL class_addProtocol(Class cls, void *proto) {
    if (!cls || !cls->rw) return NO;
    cls->rw->protocols = realloc(cls->rw->protocols, sizeof(void *) * (size_t)(cls->rw->nprotocols + 1));
    cls->rw->protocols[cls->rw->nprotocols++] = proto;
    return YES;
}
void *property_copyAttributeList(void *prop, unsigned *outCount) {
    (void)prop;
    if (outCount) *outCount = 0;
    return NULL;
}

// ---- protocols ---------------------------------------------------------------------
void *objc_getProtocol(const char *name) { (void)name; return NULL; }
const char *protocol_getName(void *p) {
    struct objc_protocol_t *pr = p;
    return pr ? pr->mangled_name : NULL;
}
typedef struct { SEL name; const char *types; } objc_method_description;
objc_method_description protocol_getMethodDescription(void *p, SEL sel, BOOL required, BOOL instance) {
    objc_method_description d = {NULL, NULL};
    struct objc_protocol_t *pr = p;
    if (!pr) return d;
    struct objc_method_list *ml = required ? (instance ? pr->instance_methods : pr->class_methods)
                                           : (instance ? pr->opt_instance_methods : pr->opt_class_methods);
    if (!ml) return d;
    uint32_t es = ml->entsize_flags & ~3u & 0xffff;
    for (uint32_t i = 0; i < ml->count; i++) {
        struct objc_method_t *m = (struct objc_method_t *)((uint8_t *)ml->first + i * es);
        if (!strcmp(m->name, sel)) { d.name = sel_registerName(m->name); d.types = m->types; return d; }
    }
    return d;
}

// ---- ARC / property helpers -----------------------------------------------------------
id objc_retainAutoreleasedReturnValue(id o) { return objc_retain(o); }
id objc_autoreleaseReturnValue(id o) { return objc_autorelease(o); }
id objc_retainAutorelease(id o) { return objc_autorelease(objc_retain(o)); }
void objc_storeStrong(id *loc, id val) {
    id old = *loc;
    val = objc_retain(val);
    *loc = val;
    objc_release(old);
}
id objc_getProperty(id self, SEL _c, ptrdiff_t off, BOOL atomic) {
    (void)_c; (void)atomic;
    id v = *(id *)((uint8_t *)self + off);
    return objc_autorelease(objc_retain(v));
}
static void set_prop(id self, ptrdiff_t off, id val, int copy) {
    id nv = copy ? (val ? ((id(*)(id, SEL))objc_msgSend)(val, sel_registerName("copy")) : nil) : objc_retain(val);
    id *slot = (id *)((uint8_t *)self + off);
    id old = *slot;
    *slot = nv;
    objc_release(old);
}
void objc_setProperty_nonatomic(id self, SEL _c, id val, ptrdiff_t off) { (void)_c; set_prop(self, off, val, 0); }
void objc_setProperty_nonatomic_copy(id self, SEL _c, id val, ptrdiff_t off) { (void)_c; set_prop(self, off, val, 1); }
void objc_setProperty_atomic(id self, SEL _c, id val, ptrdiff_t off) { (void)_c; set_prop(self, off, val, 0); }
void objc_setProperty_atomic_copy(id self, SEL _c, id val, ptrdiff_t off) { (void)_c; set_prop(self, off, val, 1); }
void objc_copyStruct(void *dst, const void *src, ptrdiff_t size, BOOL atomic, BOOL strong) {
    (void)atomic; (void)strong;
    memmove(dst, src, (size_t)size);
}

// @synchronized: one global recursive lock is plenty for this game.
static pthread_mutex_t g_sync_lock;
static pthread_once_t g_sync_once = PTHREAD_ONCE_INIT;
static void sync_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_sync_lock, &a);
}
int objc_sync_enter(id o) { (void)o; pthread_once(&g_sync_once, sync_init); pthread_mutex_lock(&g_sync_lock); return 0; }
int objc_sync_exit(id o) { (void)o; pthread_mutex_unlock(&g_sync_lock); return 0; }
void objc_enumerationMutation(id o) { LOGE("objc: collection %p mutated while enumerating", (void *)o); }

// ---- blocks ----------------------------------------------------------------------------
void *_Block_copy(const void *b) { return fw_block_copy(b); }
void _Block_release(const void *b) { fw_block_release(b); }
id objc_retainBlock(id b) { return (id)fw_block_copy(b); }
void _Block_object_assign(void *dst, const void *obj, const int flags) {
    // BLOCK_FIELD_IS_OBJECT=3, IS_BLOCK=7, IS_BYREF=8, IS_WEAK=16, BYREF_CALLER=128
    if ((flags & 7) == 3) { *(const void **)dst = obj; objc_retain((id)obj); }
    else if ((flags & 7) == 7) *(void **)dst = fw_block_copy(obj);
    else *(const void **)dst = obj;  // byref: share (no heap promotion needed for our use)
}
void _Block_object_dispose(const void *obj, const int flags) {
    if ((flags & 7) == 3) objc_release((id)obj);
    else if ((flags & 7) == 7) fw_block_release(obj);
}

// ---- misc C entry points used by the symbol table ----------------------------------------
extern void *eagl_get_proc(const char *name);
void *darwin_dlsym(void *handle, const char *name) {
    (void)handle;
    void *p = eagl_get_proc(name);
    LOGV("dlsym(%s) = %p", name, p);
    return p;
}

int darwin_vsnprintf_stack(char *buf, size_t n, const char *fmt, void *stack);
void darwin_NSLog_impl(id fmt, void *stack) {
    // NSString formats may contain %@; print the format plainly in that case.
    const char *f = ns_utf8(fmt);
    if (!f) return;
    if (strstr(f, "%@")) { LOGI("[NSLog] %s", f); return; }
    char buf[2048];
    darwin_vsnprintf_stack(buf, sizeof buf, f, stack);
    LOGI("[NSLog] %s", buf);
}
