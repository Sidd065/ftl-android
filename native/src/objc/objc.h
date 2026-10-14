// Apple-ABI (objc2, arm64) compatible Objective-C runtime used to host the
// game's classes and our replacement framework classes.
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct objc_class *Class;
typedef struct objc_object {
    Class isa;
} *id;
typedef const char *SEL;  // interned selector name
typedef void (*IMP)(void);
typedef bool BOOL;  // arm64: BOOL is bool
#define YES true
#define NO false
#define nil ((id)0)
#define Nil ((Class)0)

// ---- Apple binary structures -------------------------------------------------
struct objc_method_t {
    SEL name;
    const char *types;
    IMP imp;
};
struct objc_method_list {
    uint32_t entsize_flags;
    uint32_t count;
    struct objc_method_t first[];
};
struct objc_ivar_t {
    int32_t *offset;
    const char *name;
    const char *type;
    uint32_t alignment_raw;
    uint32_t size;
};
struct objc_ivar_list {
    uint32_t entsize;
    uint32_t count;
    struct objc_ivar_t first[];
};
struct objc_property_t {
    const char *name;
    const char *attributes;
};
struct objc_property_list {
    uint32_t entsize;
    uint32_t count;
    struct objc_property_t first[];
};
struct objc_protocol_t;
struct objc_protocol_list {
    uintptr_t count;
    struct objc_protocol_t *list[];
};
struct objc_protocol_t {
    Class isa;
    const char *mangled_name;
    struct objc_protocol_list *protocols;
    struct objc_method_list *instance_methods, *class_methods, *opt_instance_methods, *opt_class_methods;
    struct objc_property_list *instance_properties;
    uint32_t size, flags;
    const char **extended_method_types;
    const char *demangled_name;
    struct objc_property_list *class_properties;
};
struct objc_class_ro {
    uint32_t flags;
    uint32_t instance_start;
    uint32_t instance_size;
    uint32_t reserved;
    const uint8_t *ivar_layout;
    const char *name;
    struct objc_method_list *base_methods;
    struct objc_protocol_list *base_protocols;
    struct objc_ivar_list *ivars;
    const uint8_t *weak_ivar_layout;
    struct objc_property_list *base_properties;
};
#define RO_META (1u << 0)
#define RO_ROOT (1u << 1)
#define RO_HAS_CXX_STRUCTORS (1u << 2)

struct objc_cache;
struct objc_class_rw;
struct objc_class {
    Class isa;
    Class superclass;
    struct objc_cache *cache;    // our method cache (replaces _objc_empty_cache)
    struct objc_class_rw *rw;    // stored in the vtable slot
    uintptr_t bits;              // class_ro_t*
};

struct objc_category_t {
    const char *name;
    Class cls;
    struct objc_method_list *instance_methods;
    struct objc_method_list *class_methods;
    struct objc_protocol_list *protocols;
    struct objc_property_list *instance_properties;
};

struct objc_super2 {
    id receiver;
    Class current_class;
};

// ---- runtime-internal per-class data ------------------------------------------
struct objc_mtable {
    uint32_t mask;  // capacity - 1 (0 when empty)
    uint32_t count;
    struct { SEL sel; IMP imp; } *slots;
};

#define RW_REALIZED (1u << 0)
#define RW_INITIALIZING (1u << 1)
#define RW_INITIALIZED (1u << 2)
#define RW_META (1u << 3)
#define RW_NO_RC (1u << 4)        // instances are not refcounted (constant strings, blocks)
#define RW_HAS_CXX_CTOR (1u << 5)
#define RW_HAS_CXX_DTOR (1u << 6)

struct objc_ivar_info {
    const char *name, *type;
    int32_t *offset;
    uint32_t size;
};

struct objc_class_rw {
    uint32_t flags;
    uint32_t instance_size;
    const char *name;
    Class cls;
    Class nonmeta;  // for metaclasses
    const struct objc_class_ro *ro;
    struct objc_mtable methods;
    struct objc_ivar_info *ivars;
    int nivars;
    struct objc_protocol_t **protocols;
    int nprotocols;
    pthread_t init_thread;
};

// ---- public runtime API (subset used by the game + our frameworks) --------------
id objc_msgSend(id self, SEL op, ...);
id objc_msgSendSuper2(struct objc_super2 *super, SEL op, ...);
SEL sel_registerName(const char *name);
SEL sel_getUid(const char *name);
const char *sel_getName(SEL sel);
Class objc_getClass(const char *name);
Class objc_getMetaClass(const char *name);
Class objc_lookUpClass(const char *name);
Class object_getClass(id obj);
const char *class_getName(Class cls);
Class class_getSuperclass(Class cls);
BOOL class_isMetaClass(Class cls);
size_t class_getInstanceSize(Class cls);
id class_createInstance(Class cls, size_t extra);
BOOL class_addMethod(Class cls, SEL name, IMP imp, const char *types);
IMP class_replaceMethod(Class cls, SEL name, IMP imp, const char *types);
IMP class_getMethodImplementation(Class cls, SEL name);
BOOL class_respondsToSelector(Class cls, SEL sel);
id object_dispose(id obj);
void *object_getIndexedIvars(id obj);
id objc_retain(id obj);
void objc_release(id obj);
id objc_autorelease(id obj);
void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *ctx);
id objc_alloc(Class cls);
BOOL objc_isKindOfClass(id obj, Class cls);

// ---- runtime helpers for defining framework classes in C ------------------------
Class rt_define_class(const char *name, Class super, size_t instance_size);
void rt_method(Class cls, const char *sel, void *imp);       // instance method
void rt_cmethod(Class cls, const char *sel, void *imp);      // class method
void rt_set_flags(Class cls, uint32_t flags);
Class rt_metaclass(Class cls);
void rt_register_image_objc(void *image /* mo_image* */);
void rt_init(void);
id rt_alloc_object(Class cls, size_t size);  // zeroed, refcount 1
long rt_retain_count(id obj);
id rt_retain(id obj);
bool rt_release(id obj);  // returns true if the object should be deallocated
void rt_dealloc_object(id obj);
id rt_autorelease(id obj);
void rt_log_unrecognized(id self, SEL sel, Class cls);
void rt_dump_class(Class cls);

// Message send helpers with typed signatures.
#define MSG(ret, ...) ((ret(*)(id, SEL, ##__VA_ARGS__))objc_msgSend)
#define SELF_CALL_ID(obj, selname) (((id(*)(id, SEL))objc_msgSend)((id)(obj), sel_registerName(selname)))
#define SEL_(name)                                         \
    ({                                                     \
        static SEL _s;                                     \
        if (!_s) _s = sel_registerName(name);              \
        _s;                                                \
    })
// Common short-hands
static inline id msg0(id o, SEL s) { return ((id(*)(id, SEL))objc_msgSend)(o, s); }
static inline id msg1(id o, SEL s, id a) { return ((id(*)(id, SEL, id))objc_msgSend)(o, s, a); }
static inline id msg2(id o, SEL s, id a, id b) { return ((id(*)(id, SEL, id, id))objc_msgSend)(o, s, a, b); }
