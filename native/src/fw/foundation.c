// Minimal Foundation / CoreFoundation replacement (just what FTL uses, plus
// common neighbours), implemented on top of our Objective-C runtime.
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fw.h"
#include "port_log.h"

Class cls_NSObject, cls_NSString, cls_NSMutableString, cls_NSConstantString, cls_NSNumber;
Class cls_NSArray, cls_NSMutableArray, cls_NSDictionary, cls_NSMutableDictionary, cls_NSSet;
Class cls_NSEnumerator, cls_NSBundle, cls_NSNotificationCenter, cls_NSNotification, cls_NSUserDefaults;
Class cls_NSURL, cls_NSRunLoop, cls_NSAutoreleasePool, cls_NSData, cls_NSError, cls_NSProcessInfo;
Class cls_NSGlobalBlock, cls_NSStackBlock, cls_NSMallocBlock, cls_Protocol;

char g_bundle_path[512] = ".";
char g_docs_path[512] = ".";
char g_cache_path[512] = ".";

#define M(cls, sel, fn) rt_method(cls, sel, (void *)(fn))
#define CM(cls, sel, fn) rt_cmethod(cls, sel, (void *)(fn))

// ============================================================================
// NSObject
static id obj_alloc(Class self, SEL _c) { (void)_c; return class_createInstance(self, 0); }
static id obj_allocWithZone(Class self, SEL _c, void *z) { (void)_c; (void)z; return class_createInstance(self, 0); }
static id obj_new(Class self, SEL _c) {
    (void)_c;
    id o = MSG(id)((id)self, SEL_("alloc"));
    return MSG(id)(o, SEL_("init"));
}
static id obj_init(id self, SEL _c) { (void)_c; return self; }
static void obj_dealloc(id self, SEL _c) { (void)_c; rt_dealloc_object(self); }
static id obj_retain(id self, SEL _c) { (void)_c; return rt_retain(self); }
static void obj_release(id self, SEL _c) {
    (void)_c;
    if (rt_release(self)) MSG(void)(self, SEL_("dealloc"));
}
static id obj_autorelease(id self, SEL _c) { (void)_c; return rt_autorelease(self); }
static NSUInteger obj_retainCount(id self, SEL _c) { (void)_c; return (NSUInteger)rt_retain_count(self); }
static id obj_self(id self, SEL _c) { (void)_c; return self; }
static Class obj_class(id self, SEL _c) { (void)_c; return object_getClass(self); }
static Class cls_class(Class self, SEL _c) { (void)_c; return self; }
static Class obj_superclass(id self, SEL _c) { (void)_c; return object_getClass(self)->superclass; }
static Class cls_superclass(Class self, SEL _c) { (void)_c; return self->superclass; }
static BOOL obj_isKindOfClass(id self, SEL _c, Class cls) { (void)_c; return objc_isKindOfClass(self, cls); }
static BOOL obj_isMemberOfClass(id self, SEL _c, Class cls) { (void)_c; return self->isa == cls; }
static BOOL obj_respondsToSelector(id self, SEL _c, SEL s) { (void)_c; return class_respondsToSelector(object_getClass(self), s); }
static BOOL cls_instancesRespond(Class self, SEL _c, SEL s) { (void)_c; return class_respondsToSelector(self, s); }
static BOOL cls_isSubclassOfClass(Class self, SEL _c, Class other) {
    (void)_c;
    for (Class c = self; c; c = c->superclass)
        if (c == other) return YES;
    return NO;
}
static BOOL obj_conformsToProtocol(id self, SEL _c, void *p) {
    (void)_c;
    for (Class c = object_getClass(self); c && c->rw; c = c->superclass)
        for (int i = 0; i < c->rw->nprotocols; i++)
            if ((void *)c->rw->protocols[i] == p) return YES;
    return NO;
}
static BOOL obj_isEqual(id self, SEL _c, id other) { (void)_c; return self == other; }
static NSUInteger obj_hash(id self, SEL _c) { (void)_c; return (NSUInteger)(uintptr_t)self >> 4; }
static id obj_description(id self, SEL _c) {
    (void)_c;
    char buf[128];
    snprintf(buf, sizeof buf, "<%s: %p>", class_getName(object_getClass(self)), (void *)self);
    return ns_str(buf);
}
static id cls_description(Class self, SEL _c) { (void)_c; return ns_str(class_getName(self)); }
static id obj_performSelector(id self, SEL _c, SEL s) { (void)_c; return MSG(id)(self, s); }
static id obj_performSelector1(id self, SEL _c, SEL s, id a) { (void)_c; return MSG(id, id)(self, s, a); }
static id obj_performSelector2(id self, SEL _c, SEL s, id a, id b) { (void)_c; return MSG(id, id, id)(self, s, a, b); }
static void obj_performOnMain(id self, SEL _c, SEL s, id arg, BOOL wait) { (void)_c; fw_perform_on_main(self, s, arg, wait); }
static void obj_performOnMainModes(id self, SEL _c, SEL s, id arg, BOOL wait, id modes) {
    (void)_c; (void)modes;
    fw_perform_on_main(self, s, arg, wait);
}
static void obj_performAfterDelay(id self, SEL _c, SEL s, id arg, double delay) {
    (void)_c; (void)delay;
    fw_perform_on_main(self, s, arg, false);
}
static id obj_copy(id self, SEL _c) { (void)_c; return MSG(id, void *)(self, SEL_("copyWithZone:"), NULL); }
static id obj_mutableCopy(id self, SEL _c) { (void)_c; return MSG(id, void *)(self, SEL_("mutableCopyWithZone:"), NULL); }
static void *obj_zone(id self, SEL _c) { (void)self; (void)_c; return NULL; }
static BOOL obj_no(id self, SEL _c) { (void)self; (void)_c; return NO; }
static void obj_noop(id self, SEL _c) { (void)self; (void)_c; }
static id cls_retain(Class self, SEL _c) { (void)_c; return (id)self; }

// ============================================================================
// NSString
static bool is_str_class(Class c) {
    for (; c; c = c->superclass)
        if (c == cls_NSString) return true;
    return false;
}
bool ns_is_string(id o) { return o && is_str_class(o->isa); }

static id str_make(Class cls, const char *s, size_t n) {
    StrObj *o = (StrObj *)class_createInstance(cls, 0);
    char *buf = malloc(n + 1);
    memcpy(buf, s, n);
    buf[n] = 0;
    o->str = buf;
    o->length = (long)n;
    o->flags = STR_OWNED | 0x7c8;
    return (id)o;
}
id ns_str_n_new(const char *s, size_t n) { return str_make(cls_NSString, s ? s : "", s ? n : 0); }
id ns_str_new(const char *s) { return ns_str_n_new(s, s ? strlen(s) : 0); }
id ns_str(const char *s) { return rt_autorelease(ns_str_new(s)); }
const char *ns_utf8(id o) {
    if (!o) return NULL;
    if (!ns_is_string(o)) return NULL;
    return ((StrObj *)o)->str ? ((StrObj *)o)->str : "";
}
bool ns_str_eq(id a, id b) {
    const char *x = ns_utf8(a), *y = ns_utf8(b);
    if (!x || !y) return false;
    return ((StrObj *)a)->length == ((StrObj *)b)->length && !memcmp(x, y, (size_t)((StrObj *)a)->length);
}
static unsigned long str_hash_bytes(const char *s, long n) {
    unsigned long h = 5381;
    for (long i = 0; i < n; i++) h = h * 33 + (uint8_t)s[i];
    return h;
}
unsigned long ns_hash(id o) {
    if (!o) return 0;
    if (ns_is_string(o)) return str_hash_bytes(((StrObj *)o)->str, ((StrObj *)o)->length);
    return MSG(NSUInteger)(o, SEL_("hash"));
}
bool ns_equal(id a, id b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (ns_is_string(a) && ns_is_string(b)) return ns_str_eq(a, b);
    return MSG(BOOL, id)(a, SEL_("isEqual:"), b);
}

static void str_dealloc(id self, SEL _c) {
    StrObj *s = (StrObj *)self;
    if (s->flags & STR_OWNED) free((void *)s->str);
    s->str = NULL;
    obj_dealloc(self, _c);
}
// UTF-16 length of a UTF-8 string
static NSUInteger utf16_len(const char *s, long n) {
    NSUInteger len = 0;
    for (long i = 0; i < n;) {
        uint8_t c = (uint8_t)s[i];
        int k = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        len += k == 4 ? 2 : 1;
        i += k;
    }
    return len;
}
static NSUInteger str_length(id self, SEL _c) { (void)_c; return utf16_len(((StrObj *)self)->str, ((StrObj *)self)->length); }
static const char *str_UTF8String(id self, SEL _c) { (void)_c; return ns_utf8(self); }
static NSUInteger str_lengthOfBytes(id self, SEL _c, NSUInteger enc) { (void)_c; (void)enc; return (NSUInteger)((StrObj *)self)->length; }
static BOOL str_getCString(id self, SEL _c, char *buf, NSUInteger max, NSUInteger enc) {
    (void)_c; (void)enc;
    StrObj *s = (StrObj *)self;
    if ((NSUInteger)s->length + 1 > max) return NO;
    memcpy(buf, s->str, (size_t)s->length + 1);
    return YES;
}
static id str_initWithUTF8String(id self, SEL _c, const char *s) {
    (void)_c;
    StrObj *o = (StrObj *)self;
    if (!s) { MSG(void)(self, SEL_("release")); return nil; }
    size_t n = strlen(s);
    char *buf = malloc(n + 1);
    memcpy(buf, s, n + 1);
    o->str = buf; o->length = (long)n; o->flags = STR_OWNED | 0x7c8;
    return self;
}
static id str_initWithBytes(id self, SEL _c, const void *b, NSUInteger n, NSUInteger enc) {
    (void)_c;
    StrObj *o = (StrObj *)self;
    char *buf;
    size_t outn = n;
    if (enc == 10 || enc == 0x94000100 || enc == 0x90000100) {  // UTF-16 variants -> UTF-8
        const uint16_t *u = b;
        size_t cnt = n / 2;
        buf = malloc(cnt * 3 + 1);
        size_t j = 0;
        for (size_t i = 0; i < cnt; i++) {
            uint32_t cp = u[i];
            if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < cnt) { cp = 0x10000 + ((cp - 0xd800) << 10) + (u[i + 1] - 0xdc00); i++; }
            if (cp < 0x80) buf[j++] = (char)cp;
            else if (cp < 0x800) { buf[j++] = (char)(0xc0 | cp >> 6); buf[j++] = (char)(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { buf[j++] = (char)(0xe0 | cp >> 12); buf[j++] = (char)(0x80 | ((cp >> 6) & 63)); buf[j++] = (char)(0x80 | (cp & 63)); }
            else { buf[j++] = (char)(0xf0 | cp >> 18); buf[j++] = (char)(0x80 | ((cp >> 12) & 63)); buf[j++] = (char)(0x80 | ((cp >> 6) & 63)); buf[j++] = (char)(0x80 | (cp & 63)); }
        }
        outn = j;
    } else {
        buf = malloc(n + 1);
        memcpy(buf, b, n);
    }
    buf[outn] = 0;
    o->str = buf; o->length = (long)outn; o->flags = STR_OWNED | 0x7c8;
    return self;
}
static id str_initWithString(id self, SEL _c, id other) { return str_initWithUTF8String(self, _c, ns_utf8(other) ? ns_utf8(other) : ""); }
static id str_init(id self, SEL _c) { return str_initWithUTF8String(self, _c, ""); }
static id str_stringWithUTF8String(Class self, SEL _c, const char *s) {
    (void)_c;
    if (!s) return nil;
    return rt_autorelease(str_make(self, s, strlen(s)));
}
static id str_stringWithString(Class self, SEL _c, id other) {
    (void)_c;
    const char *s = ns_utf8(other);
    return rt_autorelease(str_make(self, s ? s : "", s ? strlen(s) : 0));
}
static id str_string(Class self, SEL _c) { (void)_c; return rt_autorelease(str_make(self, "", 0)); }
static id str_copy(id self, SEL _c, void *z) {
    (void)_c; (void)z;
    if (self->isa == cls_NSConstantString) return self;
    return str_make(cls_NSString, ((StrObj *)self)->str, (size_t)((StrObj *)self)->length);
}
static id str_mutableCopy(id self, SEL _c, void *z) {
    (void)_c; (void)z;
    return str_make(cls_NSMutableString, ((StrObj *)self)->str, (size_t)((StrObj *)self)->length);
}
static BOOL str_isEqual(id self, SEL _c, id o) { (void)_c; return ns_equal(self, o); }
static BOOL str_isEqualToString(id self, SEL _c, id o) { (void)_c; return ns_str_eq(self, o); }
static NSUInteger str_hash(id self, SEL _c) { (void)_c; return ns_hash(self); }
static id str_description(id self, SEL _c) { (void)_c; return self; }
static int str_intValue(id self, SEL _c) { (void)_c; return atoi(ns_utf8(self)); }
static long long str_longLongValue(id self, SEL _c) { (void)_c; return atoll(ns_utf8(self)); }
static float str_floatValue(id self, SEL _c) { (void)_c; return strtof(ns_utf8(self), NULL); }
static double str_doubleValue(id self, SEL _c) { (void)_c; return strtod(ns_utf8(self), NULL); }
static BOOL str_boolValue(id self, SEL _c) {
    (void)_c;
    const char *s = ns_utf8(self);
    while (*s == ' ') s++;
    return *s == 'Y' || *s == 'y' || *s == 'T' || *s == 't' || (*s >= '1' && *s <= '9');
}
// Version-style compare (numeric aware enough for "8.0" vs "15.0"), NSComparisonResult.
static long str_compare_impl(const char *a, const char *b, int numeric) {
    if (!numeric) {
        int r = strcmp(a, b);
        return r < 0 ? -1 : r > 0;
    }
    while (*a || *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
            if (x != y) return x < y ? -1 : 1;
        } else {
            if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
            a++; b++;
        }
    }
    return 0;
}
static long str_compare(id self, SEL _c, id o) { (void)_c; return str_compare_impl(ns_utf8(self), ns_utf8(o) ? ns_utf8(o) : "", 0); }
static long str_compareOptions(id self, SEL _c, id o, NSUInteger opts) {
    (void)_c;
    return str_compare_impl(ns_utf8(self), ns_utf8(o) ? ns_utf8(o) : "", (opts & 64) != 0);  // NSNumericSearch
}
static id str_stringByAppendingString(id self, SEL _c, id o) {
    (void)_c;
    const char *a = ns_utf8(self), *b = ns_utf8(o) ? ns_utf8(o) : "";
    size_t na = strlen(a), nb = strlen(b);
    char *t = malloc(na + nb + 1);
    memcpy(t, a, na);
    memcpy(t + na, b, nb + 1);
    id r = ns_str(t);
    free(t);
    return r;
}
static id str_stringByAppendingPathComponent(id self, SEL _c, id o) {
    (void)_c;
    const char *a = ns_utf8(self), *b = ns_utf8(o) ? ns_utf8(o) : "";
    char *t = malloc(strlen(a) + strlen(b) + 2);
    size_t na = strlen(a);
    strcpy(t, a);
    if (na && a[na - 1] != '/') strcat(t, "/");
    strcat(t, b);
    id r = ns_str(t);
    free(t);
    return r;
}
static unsigned short str_characterAtIndex(id self, SEL _c, NSUInteger i) {
    (void)_c;
    StrObj *s = (StrObj *)self;
    return i < (NSUInteger)s->length ? (unsigned char)s->str[i] : 0;
}
static const char *str_fileSystemRepresentation(id self, SEL _c) { (void)_c; return ns_utf8(self); }

// NSMutableString
static void mstr_appendString(id self, SEL _c, id o) {
    (void)_c;
    StrObj *s = (StrObj *)self;
    const char *b = ns_utf8(o) ? ns_utf8(o) : "";
    size_t nb = strlen(b);
    char *t = malloc((size_t)s->length + nb + 1);
    memcpy(t, s->str, (size_t)s->length);
    memcpy(t + s->length, b, nb + 1);
    if (s->flags & STR_OWNED) free((void *)s->str);
    s->str = t;
    s->length += (long)nb;
    s->flags |= STR_OWNED;
}
static void mstr_setString(id self, SEL _c, id o) {
    (void)_c;
    StrObj *s = (StrObj *)self;
    const char *b = ns_utf8(o) ? ns_utf8(o) : "";
    if (s->flags & STR_OWNED) free((void *)s->str);
    s->str = strdup(b);
    s->length = (long)strlen(b);
    s->flags |= STR_OWNED;
}

// ============================================================================
// NSNumber
typedef struct { Class isa; int kind; union { long long i; double d; } v; } NumObj;  // kind 0=int 1=double 2=bool
static id num_make(int kind, long long i, double d) {
    NumObj *n = (NumObj *)class_createInstance(cls_NSNumber, 0);
    n->kind = kind;
    if (kind == 1) n->v.d = d; else n->v.i = i;
    return (id)n;
}
id ns_number_int(long long v) { return rt_autorelease(num_make(0, v, 0)); }
id ns_number_double(double v) { return rt_autorelease(num_make(1, 0, v)); }
long long ns_number_ll(id o) {
    NumObj *n = (NumObj *)o;
    if (!n) return 0;
    if (n->isa != cls_NSNumber) {
        if (ns_is_string(o)) return atoll(ns_utf8(o));
        return 0;
    }
    return n->kind == 1 ? (long long)n->v.d : n->v.i;
}
static double num_d(id o) {
    NumObj *n = (NumObj *)o;
    if (ns_is_string(o)) return strtod(ns_utf8(o), NULL);
    return n->kind == 1 ? n->v.d : (double)n->v.i;
}
static id num_withInt(Class s, SEL c, int v) { (void)s; (void)c; return ns_number_int(v); }
static id num_withLong(Class s, SEL c, long v) { (void)s; (void)c; return ns_number_int(v); }
static id num_withBool(Class s, SEL c, BOOL v) { (void)s; (void)c; return rt_autorelease(num_make(2, v ? 1 : 0, 0)); }
static id num_withFloat(Class s, SEL c, float v) { (void)s; (void)c; return ns_number_double(v); }
static id num_withDouble(Class s, SEL c, double v) { (void)s; (void)c; return ns_number_double(v); }
static int num_intValue(id self, SEL _c) { (void)_c; return (int)ns_number_ll(self); }
static long num_longValue(id self, SEL _c) { (void)_c; return (long)ns_number_ll(self); }
static BOOL num_boolValue(id self, SEL _c) { (void)_c; return ns_number_ll(self) != 0 || num_d(self) != 0.0; }
static float num_floatValue(id self, SEL _c) { (void)_c; return (float)num_d(self); }
static double num_doubleValue(id self, SEL _c) { (void)_c; return num_d(self); }
static BOOL num_isEqual(id self, SEL _c, id o) {
    (void)_c;
    if (!o || o->isa != cls_NSNumber) return NO;
    return num_d(self) == num_d(o);
}
static NSUInteger num_hash(id self, SEL _c) { (void)_c; return (NSUInteger)ns_number_ll(self); }
static id num_stringValue(id self, SEL _c) {
    (void)_c;
    char b[64];
    NumObj *n = (NumObj *)self;
    if (n->kind == 1) snprintf(b, sizeof b, "%g", n->v.d); else snprintf(b, sizeof b, "%lld", n->v.i);
    return ns_str(b);
}
static id num_copy(id self, SEL _c, void *z) { (void)_c; (void)z; return rt_retain(self); }

// ============================================================================
// NSArray / NSMutableArray / NSSet
typedef struct { Class isa; id *items; size_t n, cap; } ArrObj;
static void arr_reserve(ArrObj *a, size_t n) {
    if (n <= a->cap) return;
    size_t nc = a->cap ? a->cap * 2 : 8;
    while (nc < n) nc *= 2;
    a->items = realloc(a->items, nc * sizeof(id));
    a->cap = nc;
}
static id arr_make(Class cls, id *objs, size_t n) {
    ArrObj *a = (ArrObj *)class_createInstance(cls, 0);
    arr_reserve(a, n);
    for (size_t i = 0; i < n; i++) a->items[i] = rt_retain(objs[i]) ? objs[i] : objs[i];
    for (size_t i = 0; i < n; i++) if (objs[i]) MSG(id)(objs[i], SEL_("retain"));
    // (rt_retain above is a no-op for unknown objects; the message handles subclasses with custom RR)
    for (size_t i = 0; i < n; i++) if (objs[i]) rt_release(objs[i]);  // undo the rt_retain
    a->n = n;
    return (id)a;
}
id ns_array_new(id *objs, size_t n) { return arr_make(cls_NSArray, objs, n); }
id ns_array(id *objs, size_t n) { return rt_autorelease(ns_array_new(objs, n)); }
id ns_set_new(id *objs, size_t n) { return arr_make(cls_NSSet, objs, n); }
size_t ns_array_count(id a) { return a ? ((ArrObj *)a)->n : 0; }
id ns_array_at(id a, size_t i) { return (a && i < ((ArrObj *)a)->n) ? ((ArrObj *)a)->items[i] : nil; }
void ns_marray_add(id arr, id obj) {
    ArrObj *a = (ArrObj *)arr;
    if (!obj) return;
    arr_reserve(a, a->n + 1);
    a->items[a->n++] = MSG(id)(obj, SEL_("retain"));
}
static void arr_dealloc(id self, SEL _c) {
    ArrObj *a = (ArrObj *)self;
    for (size_t i = 0; i < a->n; i++) if (a->items[i]) MSG(void)(a->items[i], SEL_("release"));
    free(a->items);
    obj_dealloc(self, _c);
}
static NSUInteger arr_count(id self, SEL _c) { (void)_c; return ((ArrObj *)self)->n; }
static id arr_objectAtIndex(id self, SEL _c, NSUInteger i) {
    (void)_c;
    ArrObj *a = (ArrObj *)self;
    if (i >= a->n) { LOGE("NSArray index %lu beyond %zu", i, a->n); return nil; }
    return a->items[i];
}
static id arr_firstObject(id self, SEL _c) { (void)_c; return ns_array_at(self, 0); }
static id arr_lastObject(id self, SEL _c) { (void)_c; ArrObj *a = (ArrObj *)self; return a->n ? a->items[a->n - 1] : nil; }
static id arr_array(Class self, SEL _c) { (void)_c; return rt_autorelease(arr_make(self, NULL, 0)); }
static id arr_arrayWithObject(Class self, SEL _c, id o) { (void)_c; return rt_autorelease(arr_make(self, &o, 1)); }
static id arr_arrayWithObjectsCount(Class self, SEL _c, id *objs, NSUInteger n) { (void)_c; return rt_autorelease(arr_make(self, objs, n)); }
static id arr_init(id self, SEL _c) { (void)_c; return self; }
static void marr_addObject(id self, SEL _c, id o) { (void)_c; ns_marray_add(self, o); }
static void marr_removeObjectAtIndex(id self, SEL _c, NSUInteger i) {
    (void)_c;
    ArrObj *a = (ArrObj *)self;
    if (i >= a->n) return;
    id o = a->items[i];
    memmove(a->items + i, a->items + i + 1, (a->n - i - 1) * sizeof(id));
    a->n--;
    if (o) MSG(void)(o, SEL_("release"));
}
static void marr_removeObject(id self, SEL _c, id o) {
    ArrObj *a = (ArrObj *)self;
    for (size_t i = a->n; i-- > 0;)
        if (ns_equal(a->items[i], o)) marr_removeObjectAtIndex(self, _c, i);
}
static void marr_removeAllObjects(id self, SEL _c) {
    ArrObj *a = (ArrObj *)self;
    while (a->n) marr_removeObjectAtIndex(self, _c, a->n - 1);
}
static BOOL arr_containsObject(id self, SEL _c, id o) {
    (void)_c;
    ArrObj *a = (ArrObj *)self;
    for (size_t i = 0; i < a->n; i++) if (ns_equal(a->items[i], o)) return YES;
    return NO;
}
static id arr_anyObject(id self, SEL _c) { (void)_c; return ns_array_at(self, 0); }
static id arr_allObjects(id self, SEL _c) { (void)_c; ArrObj *a = (ArrObj *)self; return ns_array(a->items, a->n); }
static id arr_copy(id self, SEL _c, void *z) { (void)_c; (void)z; ArrObj *a = (ArrObj *)self; return arr_make(self->isa == cls_NSMutableArray ? cls_NSArray : self->isa, a->items, a->n); }
static id arr_mutableCopy(id self, SEL _c, void *z) { (void)_c; (void)z; ArrObj *a = (ArrObj *)self; return arr_make(cls_NSMutableArray, a->items, a->n); }

// fast enumeration: countByEnumeratingWithState:objects:count:
typedef struct { unsigned long state; id *itemsPtr; unsigned long *mutationsPtr; unsigned long extra[5]; } NSFastEnumerationState;
static unsigned long g_mutations;
static NSUInteger arr_countByEnumerating(id self, SEL _c, NSFastEnumerationState *st, id *buf, NSUInteger len) {
    (void)_c; (void)buf; (void)len;
    ArrObj *a = (ArrObj *)self;
    if (st->state) return 0;
    st->state = 1;
    st->itemsPtr = a->items;
    st->mutationsPtr = &g_mutations;
    return a->n;
}

// NSEnumerator
typedef struct { Class isa; id coll; size_t idx; int kind; } EnumObj;  // kind 0 items, 1 dict keys, 2 dict vals
static id enum_make(id coll, int kind) {
    EnumObj *e = (EnumObj *)class_createInstance(cls_NSEnumerator, 0);
    e->coll = MSG(id)(coll, SEL_("retain"));
    e->kind = kind;
    return rt_autorelease((id)e);
}
typedef struct { Class isa; id *keys; id *vals; size_t n, cap; } DictObj;
static id enum_nextObject(id self, SEL _c) {
    (void)_c;
    EnumObj *e = (EnumObj *)self;
    if (e->kind == 0) {
        ArrObj *a = (ArrObj *)e->coll;
        return e->idx < a->n ? a->items[e->idx++] : nil;
    }
    DictObj *d = (DictObj *)e->coll;
    if (e->idx >= d->n) return nil;
    return e->kind == 1 ? d->keys[e->idx++] : d->vals[e->idx++];
}
static id enum_allObjects(id self, SEL _c) {
    id out = rt_autorelease(arr_make(cls_NSMutableArray, NULL, 0));
    id o;
    while ((o = enum_nextObject(self, _c))) ns_marray_add(out, o);
    return out;
}
static void enum_dealloc(id self, SEL _c) {
    EnumObj *e = (EnumObj *)self;
    if (e->coll) MSG(void)(e->coll, SEL_("release"));
    obj_dealloc(self, _c);
}
static id arr_objectEnumerator(id self, SEL _c) { (void)_c; return enum_make(self, 0); }

// ============================================================================
// NSDictionary / NSMutableDictionary (small; linear probing is plenty)
static void dict_reserve(DictObj *d, size_t n) {
    if (n <= d->cap) return;
    size_t nc = d->cap ? d->cap * 2 : 8;
    while (nc < n) nc *= 2;
    d->keys = realloc(d->keys, nc * sizeof(id));
    d->vals = realloc(d->vals, nc * sizeof(id));
    d->cap = nc;
}
static long dict_find(DictObj *d, id key) {
    for (size_t i = 0; i < d->n; i++) if (ns_equal(d->keys[i], key)) return (long)i;
    return -1;
}
id ns_dict_new(void) { return class_createInstance(cls_NSMutableDictionary, 0); }
void ns_dict_set(id dict, id key, id val) {
    DictObj *d = (DictObj *)dict;
    if (!key) return;
    long i = dict_find(d, key);
    if (val) MSG(id)(val, SEL_("retain"));
    if (i >= 0) {
        if (d->vals[i]) MSG(void)(d->vals[i], SEL_("release"));
        d->vals[i] = val;
        return;
    }
    dict_reserve(d, d->n + 1);
    d->keys[d->n] = ns_is_string(key) ? str_copy(key, NULL, NULL) : MSG(id)(key, SEL_("retain"));
    d->vals[d->n] = val;
    d->n++;
}
id ns_dict_get(id dict, id key) {
    if (!dict || !key) return nil;
    DictObj *d = (DictObj *)dict;
    long i = dict_find(d, key);
    return i >= 0 ? d->vals[i] : nil;
}
static void dict_remove(DictObj *d, id key) {
    long i = dict_find(d, key);
    if (i < 0) return;
    id k = d->keys[i], v = d->vals[i];
    memmove(d->keys + i, d->keys + i + 1, (d->n - (size_t)i - 1) * sizeof(id));
    memmove(d->vals + i, d->vals + i + 1, (d->n - (size_t)i - 1) * sizeof(id));
    d->n--;
    if (k) MSG(void)(k, SEL_("release"));
    if (v) MSG(void)(v, SEL_("release"));
}
static void dict_dealloc(id self, SEL _c) {
    DictObj *d = (DictObj *)self;
    for (size_t i = 0; i < d->n; i++) {
        if (d->keys[i]) MSG(void)(d->keys[i], SEL_("release"));
        if (d->vals[i]) MSG(void)(d->vals[i], SEL_("release"));
    }
    free(d->keys);
    free(d->vals);
    obj_dealloc(self, _c);
}
static id dict_objectForKey(id self, SEL _c, id key) { (void)_c; return ns_dict_get(self, key); }
static id dict_valueForKey(id self, SEL _c, id key) { (void)_c; return ns_dict_get(self, key); }
static NSUInteger dict_count(id self, SEL _c) { (void)_c; return ((DictObj *)self)->n; }
static id dict_keyEnumerator(id self, SEL _c) { (void)_c; return enum_make(self, 1); }
static id dict_objectEnumerator(id self, SEL _c) { (void)_c; return enum_make(self, 2); }
static id dict_allKeys(id self, SEL _c) { (void)_c; DictObj *d = (DictObj *)self; return ns_array(d->keys, d->n); }
static id dict_allValues(id self, SEL _c) { (void)_c; DictObj *d = (DictObj *)self; return ns_array(d->vals, d->n); }
static id dict_make(Class cls, id *objs, id *keys, size_t n) {
    id d = class_createInstance(cls, 0);
    for (size_t i = 0; i < n; i++) ns_dict_set(d, keys[i], objs[i]);
    return d;
}
static id dict_dictionary(Class self, SEL _c) { (void)_c; return rt_autorelease(class_createInstance(self, 0)); }
static id dict_withObjectsForKeysCount(Class self, SEL _c, id *objs, id *keys, NSUInteger n) {
    (void)_c;
    return rt_autorelease(dict_make(self, objs, keys, n));
}
static id dict_withObjectForKey(Class self, SEL _c, id obj, id key) { (void)_c; return rt_autorelease(dict_make(self, &obj, &key, 1)); }
static id dict_withDictionary(Class self, SEL _c, id other) {
    (void)_c;
    DictObj *o = (DictObj *)other;
    return rt_autorelease(o ? dict_make(self, o->vals, o->keys, o->n) : class_createInstance(self, 0));
}
static id dict_init(id self, SEL _c) { (void)_c; return self; }
static id dict_initWithCapacity(id self, SEL _c, NSUInteger n) { (void)_c; dict_reserve((DictObj *)self, n); return self; }
static void mdict_setObjectForKey(id self, SEL _c, id obj, id key) { (void)_c; ns_dict_set(self, key, obj); }
static void mdict_removeObjectForKey(id self, SEL _c, id key) { (void)_c; dict_remove((DictObj *)self, key); }
static void mdict_removeAll(id self, SEL _c) {
    (void)_c;
    DictObj *d = (DictObj *)self;
    while (d->n) dict_remove(d, d->keys[d->n - 1]);
}
static void mdict_addEntries(id self, SEL _c, id other) {
    (void)_c;
    DictObj *o = (DictObj *)other;
    if (!o) return;
    for (size_t i = 0; i < o->n; i++) ns_dict_set(self, o->keys[i], o->vals[i]);
}
static id dict_copy(id self, SEL _c, void *z) { (void)_c; (void)z; DictObj *d = (DictObj *)self; return dict_make(cls_NSDictionary, d->vals, d->keys, d->n); }
static id dict_mutableCopy(id self, SEL _c, void *z) { (void)_c; (void)z; DictObj *d = (DictObj *)self; return dict_make(cls_NSMutableDictionary, d->vals, d->keys, d->n); }
static NSUInteger dict_countByEnumerating(id self, SEL _c, NSFastEnumerationState *st, id *buf, NSUInteger len) {
    (void)_c; (void)buf; (void)len;
    DictObj *d = (DictObj *)self;
    if (st->state) return 0;
    st->state = 1;
    st->itemsPtr = d->keys;
    st->mutationsPtr = &g_mutations;
    return d->n;
}

// ============================================================================
// NSNotificationCenter
typedef struct observer { id obs; SEL sel; id name; id object; struct observer *next; } observer;
static observer *g_observers;
static pthread_mutex_t g_obs_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { Class isa; id name, object, userInfo; } NoteObj;
static id g_default_center;
static id nc_defaultCenter(Class self, SEL _c) {
    (void)_c;
    if (!g_default_center) g_default_center = class_createInstance(self, 0);
    return g_default_center;
}
static void nc_addObserver(id self, SEL _c, id obs, SEL sel, id name, id object) {
    (void)self; (void)_c;
    observer *o = calloc(1, sizeof(*o));
    o->obs = obs;
    o->sel = sel;
    o->name = name ? MSG(id)(name, SEL_("retain")) : nil;
    o->object = object;
    pthread_mutex_lock(&g_obs_lock);
    o->next = g_observers;
    g_observers = o;
    pthread_mutex_unlock(&g_obs_lock);
    LOGV("NSNotificationCenter: %s observes %s", class_getName(object_getClass(obs)), name ? ns_utf8(name) : "*");
}
static void nc_removeObserver(id self, SEL _c, id obs) {
    (void)self; (void)_c;
    pthread_mutex_lock(&g_obs_lock);
    for (observer **pp = &g_observers; *pp;) {
        if ((*pp)->obs == obs) { observer *d = *pp; *pp = d->next; free(d); }
        else pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_obs_lock);
}
static void nc_removeObserverName(id self, SEL _c, id obs, id name, id object) {
    (void)self; (void)_c; (void)object;
    pthread_mutex_lock(&g_obs_lock);
    for (observer **pp = &g_observers; *pp;) {
        if ((*pp)->obs == obs && (!name || ns_equal((*pp)->name, name))) { observer *d = *pp; *pp = d->next; free(d); }
        else pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_obs_lock);
}
void ns_post_notification(id name, id object, id userinfo) {
    NoteObj *n = (NoteObj *)class_createInstance(cls_NSNotification, 0);
    n->name = name;
    n->object = object;
    n->userInfo = userinfo;
    observer *list[64];
    int cnt = 0;
    pthread_mutex_lock(&g_obs_lock);
    for (observer *o = g_observers; o && cnt < 64; o = o->next)
        if ((!o->name || ns_equal(o->name, name)) && (!o->object || o->object == object)) list[cnt++] = o;
    pthread_mutex_unlock(&g_obs_lock);
    for (int i = 0; i < cnt; i++) MSG(void, id)(list[i]->obs, list[i]->sel, (id)n);
    MSG(void)((id)n, SEL_("release"));
}
static void nc_post(id self, SEL _c, id name, id object) { (void)self; (void)_c; ns_post_notification(name, object, nil); }
static void nc_postUserInfo(id self, SEL _c, id name, id object, id ui) { (void)self; (void)_c; ns_post_notification(name, object, ui); }
static id note_name(id self, SEL _c) { (void)_c; return ((NoteObj *)self)->name; }
static id note_object(id self, SEL _c) { (void)_c; return ((NoteObj *)self)->object; }
static id note_userInfo(id self, SEL _c) { (void)_c; return ((NoteObj *)self)->userInfo; }

// ============================================================================
// NSUserDefaults (persisted as simple "key\ttype\tvalue" lines)
typedef struct { Class isa; id values; id registered; } DefaultsObj;
static id g_defaults;
static char g_defaults_file[600];
static void defaults_save(void) {
    if (!g_defaults) return;
    DictObj *d = (DictObj *)((DefaultsObj *)g_defaults)->values;
    FILE *f = fopen(g_defaults_file, "w");
    if (!f) return;
    for (size_t i = 0; i < d->n; i++) {
        const char *k = ns_utf8(d->keys[i]);
        id v = d->vals[i];
        if (!k || !v) continue;
        if (ns_is_string(v)) fprintf(f, "%s\ts\t%s\n", k, ns_utf8(v));
        else if (v->isa == cls_NSNumber) fprintf(f, "%s\tn\t%.17g\n", k, num_d(v));
    }
    fclose(f);
}
static void defaults_load(DefaultsObj *u) {
    FILE *f = fopen(g_defaults_file, "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t2 = 0;
        char *val = t2 + 1;
        val[strcspn(val, "\n")] = 0;
        id key = ns_str(line);
        if (t1[1] == 's') ns_dict_set(u->values, key, ns_str(val));
        else ns_dict_set(u->values, key, ns_number_double(strtod(val, NULL)));
    }
    fclose(f);
}
static id ud_standard(Class self, SEL _c) {
    (void)_c;
    if (!g_defaults) {
        DefaultsObj *u = (DefaultsObj *)class_createInstance(self, 0);
        u->values = ns_dict_new();
        u->registered = ns_dict_new();
        snprintf(g_defaults_file, sizeof g_defaults_file, "%s/userdefaults.txt", g_docs_path);
        defaults_load(u);
        g_defaults = (id)u;
    }
    return g_defaults;
}
static id ud_get(id self, id key) {
    DefaultsObj *u = (DefaultsObj *)self;
    id v = ns_dict_get(u->values, key);
    return v ? v : ns_dict_get(u->registered, key);
}
static void ud_registerDefaults(id self, SEL _c, id dict) { (void)_c; mdict_addEntries(((DefaultsObj *)self)->registered, NULL, dict); }
static id ud_objectForKey(id self, SEL _c, id key) { (void)_c; return ud_get(self, key); }
static BOOL ud_boolForKey(id self, SEL _c, id key) {
    (void)_c;
    id v = ud_get(self, key);
    if (!v) return NO;
    if (ns_is_string(v)) return str_boolValue(v, NULL);
    return num_d(v) != 0;
}
static NSInteger ud_integerForKey(id self, SEL _c, id key) { (void)_c; return (NSInteger)ns_number_ll(ud_get(self, key)); }
static float ud_floatForKey(id self, SEL _c, id key) { (void)_c; id v = ud_get(self, key); return v ? (float)num_d(v) : 0.f; }
static id ud_stringForKey(id self, SEL _c, id key) { (void)_c; id v = ud_get(self, key); return ns_is_string(v) ? v : nil; }
static void ud_setObject(id self, SEL _c, id obj, id key) {
    (void)_c;
    if (!obj) dict_remove((DictObj *)((DefaultsObj *)self)->values, key);
    else ns_dict_set(((DefaultsObj *)self)->values, key, obj);
}
static void ud_setBool(id self, SEL _c, BOOL v, id key) { ud_setObject(self, _c, rt_autorelease(num_make(2, v, 0)), key); }
static void ud_setInteger(id self, SEL _c, NSInteger v, id key) { ud_setObject(self, _c, ns_number_int(v), key); }
static void ud_setFloat(id self, SEL _c, float v, id key) { ud_setObject(self, _c, ns_number_double(v), key); }
static void ud_removeObjectForKey(id self, SEL _c, id key) { (void)_c; dict_remove((DictObj *)((DefaultsObj *)self)->values, key); }
static BOOL ud_synchronize(id self, SEL _c) { (void)self; (void)_c; defaults_save(); return YES; }

// ============================================================================
// NSBundle
typedef struct { Class isa; id path; id info; } BundleObj;
static id g_main_bundle;
static id bundle_main(Class self, SEL _c) {
    (void)_c;
    if (!g_main_bundle) {
        BundleObj *b = (BundleObj *)class_createInstance(self, 0);
        b->path = ns_str_new(g_bundle_path);
        id info = ns_dict_new();
        ns_dict_set(info, ns_str("CFBundleName"), ns_str("FTL"));
        ns_dict_set(info, ns_str("CFBundleDisplayName"), ns_str("FTL"));
        ns_dict_set(info, ns_str("CFBundleExecutable"), ns_str("FTL"));
        ns_dict_set(info, ns_str("CFBundleIdentifier"), ns_str("com.ftlgame.FTL"));
        ns_dict_set(info, ns_str("CFBundleShortVersionString"), ns_str("1.6.21"));
        ns_dict_set(info, ns_str("CFBundleVersion"), ns_str("1"));
        b->info = info;
        g_main_bundle = (id)b;
    }
    return g_main_bundle;
}
static id bundle_resourcePath(id self, SEL _c) { (void)_c; return ((BundleObj *)self)->path; }
static id bundle_infoDictionary(id self, SEL _c) { (void)_c; return ((BundleObj *)self)->info; }
static id bundle_objectForInfoKey(id self, SEL _c, id key) { (void)_c; return ns_dict_get(((BundleObj *)self)->info, key); }
static id bundle_pathForResource(id self, SEL _c, id name, id type) {
    (void)_c;
    char p[1024];
    const char *t = ns_utf8(type);
    snprintf(p, sizeof p, "%s/%s%s%s", ns_utf8(((BundleObj *)self)->path), ns_utf8(name), t && *t ? "." : "", t ? t : "");
    struct stat st;
    if (stat(p, &st) != 0) return nil;
    return ns_str(p);
}
static id bundle_localizedString(id self, SEL _c, id key, id value, id table) {
    (void)self; (void)_c; (void)table;
    return value && ns_utf8(value) && *ns_utf8(value) ? value : key;
}

// ============================================================================
// NSURL, NSRunLoop, NSAutoreleasePool, NSProcessInfo, NSError
typedef struct { Class isa; id str; } URLObj;
static id url_withString(Class self, SEL _c, id s) {
    (void)_c;
    if (!s) return nil;
    URLObj *u = (URLObj *)class_createInstance(self, 0);
    u->str = MSG(id)(s, SEL_("copy"));
    return rt_autorelease((id)u);
}
static id url_absoluteString(id self, SEL _c) { (void)_c; return ((URLObj *)self)->str; }

static id g_main_runloop;
static id rl_currentRunLoop(Class self, SEL _c) {
    (void)_c;
    if (!g_main_runloop) g_main_runloop = class_createInstance(self, 0);
    return g_main_runloop;
}

typedef struct { Class isa; void *ctx; } PoolObj;
static id pool_init(id self, SEL _c) { (void)_c; ((PoolObj *)self)->ctx = objc_autoreleasePoolPush(); return self; }
static void pool_drain(id self, SEL _c) {
    (void)_c;
    objc_autoreleasePoolPop(((PoolObj *)self)->ctx);
    rt_dealloc_object(self);
}
static void pool_addObject(Class self, SEL _c, id o) { (void)self; (void)_c; rt_autorelease(o); }

static id pi_processInfo(Class self, SEL _c) {
    (void)_c;
    static id pi;
    if (!pi) pi = class_createInstance(self, 0);
    return pi;
}
static NSUInteger pi_processorCount(id self, SEL _c) { (void)self; (void)_c; return 4; }
static unsigned long long pi_physicalMemory(id self, SEL _c) { (void)self; (void)_c; return 4ull << 30; }

// ============================================================================
// Blocks
void *fw_block_copy(const void *b) {
    struct Block_layout *blk = (struct Block_layout *)b;
    if (!blk) return NULL;
    if (blk->isa == (void *)cls_NSGlobalBlock) return (void *)b;
    if (blk->isa == (void *)cls_NSMallocBlock) { __atomic_add_fetch(&blk->flags, 2, __ATOMIC_RELAXED); return (void *)b; }
    struct Block_layout *n = malloc(blk->descriptor->size);
    memcpy(n, blk, blk->descriptor->size);
    n->isa = cls_NSMallocBlock;
    n->flags = (blk->flags & ~0xfffe) | (1 << 24) | 2;  // NEEDS_FREE, rc=1
    if (blk->flags & (1 << 25)) n->descriptor->copy(n, blk);
    return n;
}
void fw_block_release(const void *b) {
    struct Block_layout *blk = (struct Block_layout *)b;
    if (!blk || blk->isa != (void *)cls_NSMallocBlock) return;
    if ((__atomic_sub_fetch(&blk->flags, 2, __ATOMIC_ACQ_REL) & 0xfffe) == 0) {
        if (blk->flags & (1 << 25)) blk->descriptor->dispose(blk);
        free(blk);
    }
}
static id blk_copy(id self, SEL _c) { (void)_c; return (id)fw_block_copy(self); }
static void blk_release(id self, SEL _c) { (void)_c; fw_block_release(self); }
static id blk_retain(id self, SEL _c) {
    (void)_c;
    struct Block_layout *blk = (struct Block_layout *)self;
    if (blk->isa == (void *)cls_NSMallocBlock) __atomic_add_fetch(&blk->flags, 2, __ATOMIC_RELAXED);
    return self;
}

// ============================================================================
// C API: CoreFoundation + Foundation functions
id CFBundleGetMainBundle_impl(void) { return bundle_main(cls_NSBundle, NULL); }
id CFBundleGetInfoDictionary_impl(id b) { return b ? ((BundleObj *)b)->info : nil; }
id CFBundleCopyLocalizedString_impl(id bundle, id key, id value, id table) {
    (void)bundle; (void)table;
    id r = (value && ns_utf8(value) && *ns_utf8(value)) ? value : key;
    return r ? MSG(id)(r, SEL_("retain")) : nil;
}
void CFRelease_impl(id o) { if (o) MSG(void)(o, SEL_("release")); }
id CFRetain_impl(id o) { return o ? MSG(id)(o, SEL_("retain")) : o; }
id CFStringCreateWithCStringNoCopy_impl(void *alloc, const char *s, uint32_t enc, void *dealloc) {
    (void)alloc; (void)enc; (void)dealloc;
    return s ? ns_str_new(s) : nil;
}
id CFStringCreateWithCString_impl(void *alloc, const char *s, uint32_t enc) {
    (void)alloc; (void)enc;
    return s ? ns_str_new(s) : nil;
}
BOOL CFStringGetCString_impl(id s, char *buf, long size, uint32_t enc) {
    (void)enc;
    const char *u = ns_utf8(s);
    if (!u || size <= 0) return NO;
    size_t n = strlen(u);
    if ((long)n + 1 > size) { memcpy(buf, u, (size_t)size - 1); buf[size - 1] = 0; return NO; }
    memcpy(buf, u, n + 1);
    return YES;
}
const char *CFStringGetCStringPtr_impl(id s, uint32_t enc) { (void)enc; return ns_utf8(s); }
long CFStringGetLength_impl(id s) { return s ? (long)str_length(s, NULL) : 0; }

static const char *g_hidden_classes[] = {"GKLocalPlayer", "GKAchievement", "GKGameCenterViewController", NULL};
Class NSClassFromString_impl(id name) {
    const char *n = ns_utf8(name);
    if (!n) return Nil;
    for (int i = 0; g_hidden_classes[i]; i++)
        if (!strcmp(n, g_hidden_classes[i])) return Nil;  // Game Center is not available
    return objc_getClass(n);
}
id NSStringFromClass_impl(Class c) { return c ? ns_str(class_getName(c)) : nil; }
id NSStringFromSelector_impl(SEL s) { return s ? ns_str(s) : nil; }
SEL NSSelectorFromString_impl(id s) { return ns_utf8(s) ? sel_registerName(ns_utf8(s)) : NULL; }

id NSSearchPathForDirectoriesInDomains_impl(NSUInteger dir, NSUInteger mask, BOOL expand) {
    (void)mask; (void)expand;
    const char *base = g_docs_path;
    if (dir == 13) base = g_cache_path;  // NSCachesDirectory
    char p[600];
    snprintf(p, sizeof p, "%s", base);
    mkdir(p, 0755);
    id s = ns_str(p);
    LOGI("NSSearchPathForDirectoriesInDomains(%lu) -> %s", dir, p);
    return ns_array(&s, 1);
}
id NSHomeDirectory_impl(void) { return ns_str(g_docs_path); }
id NSTemporaryDirectory_impl(void) { return ns_str(g_cache_path); }

void NSLog_impl_cstr(const char *msg) { LOGI("[NSLog] %s", msg); }

// constant NSString globals
id g_NSDefaultRunLoopMode, g_NSRunLoopCommonModes;
uint64_t g_kCFAllocatorNull[2];
double g_kCFCoreFoundationVersionNumber = 1854.0;  // iOS 15
void *g_kCFAllocatorNullPtr = g_kCFAllocatorNull;

// ============================================================================
void fw_foundation_init(void) {
    cls_NSObject = rt_define_class("NSObject", Nil, sizeof(struct objc_object));
    Class o = cls_NSObject;
    CM(o, "alloc", obj_alloc); CM(o, "allocWithZone:", obj_allocWithZone); CM(o, "new", obj_new);
    CM(o, "class", cls_class); CM(o, "superclass", cls_superclass); CM(o, "initialize", obj_noop); CM(o, "load", obj_noop);
    CM(o, "instancesRespondToSelector:", cls_instancesRespond); CM(o, "isSubclassOfClass:", cls_isSubclassOfClass);
    CM(o, "description", cls_description); CM(o, "retain", cls_retain); CM(o, "autorelease", cls_retain);
    CM(o, "release", obj_noop); CM(o, "copy", cls_retain);
    M(o, "init", obj_init); M(o, "dealloc", obj_dealloc); M(o, "retain", obj_retain); M(o, "release", obj_release);
    M(o, "autorelease", obj_autorelease); M(o, "retainCount", obj_retainCount); M(o, "self", obj_self);
    M(o, "class", obj_class); M(o, "superclass", obj_superclass); M(o, "isKindOfClass:", obj_isKindOfClass);
    M(o, "isMemberOfClass:", obj_isMemberOfClass); M(o, "respondsToSelector:", obj_respondsToSelector);
    M(o, "conformsToProtocol:", obj_conformsToProtocol); M(o, "isEqual:", obj_isEqual); M(o, "hash", obj_hash);
    M(o, "description", obj_description); M(o, "debugDescription", obj_description);
    M(o, "performSelector:", obj_performSelector); M(o, "performSelector:withObject:", obj_performSelector1);
    M(o, "performSelector:withObject:withObject:", obj_performSelector2);
    M(o, "performSelectorOnMainThread:withObject:waitUntilDone:", obj_performOnMain);
    M(o, "performSelectorOnMainThread:withObject:waitUntilDone:modes:", obj_performOnMainModes);
    M(o, "performSelector:withObject:afterDelay:", obj_performAfterDelay);
    M(o, "copy", obj_copy); M(o, "mutableCopy", obj_mutableCopy); M(o, "zone", obj_zone); M(o, "isProxy", obj_no);

    cls_NSString = rt_define_class("NSString", cls_NSObject, sizeof(StrObj));
    cls_NSMutableString = rt_define_class("NSMutableString", cls_NSString, sizeof(StrObj));
    cls_NSConstantString = rt_define_class("__NSCFConstantString", cls_NSString, sizeof(StrObj));
    rt_set_flags(cls_NSConstantString, RW_NO_RC);
    Class s = cls_NSString;
    M(s, "dealloc", str_dealloc); M(s, "length", str_length); M(s, "UTF8String", str_UTF8String);
    M(s, "cString", str_UTF8String); M(s, "fileSystemRepresentation", str_fileSystemRepresentation);
    M(s, "lengthOfBytesUsingEncoding:", str_lengthOfBytes); M(s, "maximumLengthOfBytesUsingEncoding:", str_lengthOfBytes);
    M(s, "getCString:maxLength:encoding:", str_getCString); M(s, "initWithUTF8String:", str_initWithUTF8String);
    M(s, "initWithBytes:length:encoding:", str_initWithBytes); M(s, "initWithString:", str_initWithString);
    M(s, "init", str_init); M(s, "copyWithZone:", str_copy); M(s, "mutableCopyWithZone:", str_mutableCopy);
    M(s, "isEqual:", str_isEqual); M(s, "isEqualToString:", str_isEqualToString); M(s, "hash", str_hash);
    M(s, "description", str_description); M(s, "intValue", str_intValue); M(s, "integerValue", str_longLongValue);
    M(s, "longLongValue", str_longLongValue); M(s, "floatValue", str_floatValue); M(s, "doubleValue", str_doubleValue);
    M(s, "boolValue", str_boolValue); M(s, "compare:", str_compare); M(s, "compare:options:", str_compareOptions);
    M(s, "stringByAppendingString:", str_stringByAppendingString);
    M(s, "stringByAppendingPathComponent:", str_stringByAppendingPathComponent);
    M(s, "characterAtIndex:", str_characterAtIndex);
    CM(s, "stringWithUTF8String:", str_stringWithUTF8String); CM(s, "stringWithCString:encoding:", str_stringWithUTF8String);
    CM(s, "stringWithString:", str_stringWithString); CM(s, "string", str_string);
    M(cls_NSMutableString, "appendString:", mstr_appendString); M(cls_NSMutableString, "setString:", mstr_setString);
    M(cls_NSConstantString, "retain", obj_self); M(cls_NSConstantString, "autorelease", obj_self);
    M(cls_NSConstantString, "release", obj_noop); M(cls_NSConstantString, "dealloc", obj_noop);

    cls_NSNumber = rt_define_class("NSNumber", cls_NSObject, sizeof(NumObj));
    Class n = cls_NSNumber;
    CM(n, "numberWithInt:", num_withInt); CM(n, "numberWithInteger:", num_withLong); CM(n, "numberWithLong:", num_withLong);
    CM(n, "numberWithLongLong:", num_withLong); CM(n, "numberWithUnsignedInt:", num_withInt);
    CM(n, "numberWithBool:", num_withBool); CM(n, "numberWithFloat:", num_withFloat); CM(n, "numberWithDouble:", num_withDouble);
    M(n, "intValue", num_intValue); M(n, "integerValue", num_longValue); M(n, "longValue", num_longValue);
    M(n, "longLongValue", num_longValue); M(n, "unsignedIntValue", num_intValue); M(n, "boolValue", num_boolValue);
    M(n, "floatValue", num_floatValue); M(n, "doubleValue", num_doubleValue); M(n, "isEqual:", num_isEqual);
    M(n, "hash", num_hash); M(n, "stringValue", num_stringValue); M(n, "description", num_stringValue);
    M(n, "copyWithZone:", num_copy);

    cls_NSArray = rt_define_class("NSArray", cls_NSObject, sizeof(ArrObj));
    cls_NSMutableArray = rt_define_class("NSMutableArray", cls_NSArray, sizeof(ArrObj));
    cls_NSSet = rt_define_class("NSSet", cls_NSObject, sizeof(ArrObj));
    Class sets[] = {cls_NSArray, cls_NSSet};
    for (int i = 0; i < 2; i++) {
        Class a = sets[i];
        M(a, "dealloc", arr_dealloc); M(a, "count", arr_count); M(a, "objectEnumerator", arr_objectEnumerator);
        M(a, "countByEnumeratingWithState:objects:count:", arr_countByEnumerating); M(a, "init", arr_init);
        M(a, "containsObject:", arr_containsObject); M(a, "anyObject", arr_anyObject); M(a, "allObjects", arr_allObjects);
        M(a, "copyWithZone:", arr_copy); M(a, "mutableCopyWithZone:", arr_mutableCopy);
        CM(a, "array", arr_array); CM(a, "set", arr_array); CM(a, "arrayWithObject:", arr_arrayWithObject);
        CM(a, "setWithObject:", arr_arrayWithObject); CM(a, "arrayWithObjects:count:", arr_arrayWithObjectsCount);
    }
    M(cls_NSArray, "objectAtIndex:", arr_objectAtIndex); M(cls_NSArray, "objectAtIndexedSubscript:", arr_objectAtIndex);
    M(cls_NSArray, "firstObject", arr_firstObject); M(cls_NSArray, "lastObject", arr_lastObject);
    M(cls_NSMutableArray, "addObject:", marr_addObject); M(cls_NSMutableArray, "removeObjectAtIndex:", marr_removeObjectAtIndex);
    M(cls_NSMutableArray, "removeObject:", marr_removeObject); M(cls_NSMutableArray, "removeAllObjects", marr_removeAllObjects);

    cls_NSEnumerator = rt_define_class("NSEnumerator", cls_NSObject, sizeof(EnumObj));
    M(cls_NSEnumerator, "nextObject", enum_nextObject); M(cls_NSEnumerator, "allObjects", enum_allObjects);
    M(cls_NSEnumerator, "dealloc", enum_dealloc);

    cls_NSDictionary = rt_define_class("NSDictionary", cls_NSObject, sizeof(DictObj));
    cls_NSMutableDictionary = rt_define_class("NSMutableDictionary", cls_NSDictionary, sizeof(DictObj));
    Class d = cls_NSDictionary;
    M(d, "dealloc", dict_dealloc); M(d, "objectForKey:", dict_objectForKey); M(d, "objectForKeyedSubscript:", dict_objectForKey);
    M(d, "valueForKey:", dict_valueForKey); M(d, "count", dict_count); M(d, "keyEnumerator", dict_keyEnumerator);
    M(d, "objectEnumerator", dict_objectEnumerator); M(d, "allKeys", dict_allKeys); M(d, "allValues", dict_allValues);
    M(d, "init", dict_init); M(d, "initWithCapacity:", dict_initWithCapacity); M(d, "copyWithZone:", dict_copy);
    M(d, "mutableCopyWithZone:", dict_mutableCopy); M(d, "countByEnumeratingWithState:objects:count:", dict_countByEnumerating);
    CM(d, "dictionary", dict_dictionary); CM(d, "dictionaryWithObjects:forKeys:count:", dict_withObjectsForKeysCount);
    CM(d, "dictionaryWithObject:forKey:", dict_withObjectForKey); CM(d, "dictionaryWithDictionary:", dict_withDictionary);
    Class md = cls_NSMutableDictionary;
    M(md, "setObject:forKey:", mdict_setObjectForKey); M(md, "setObject:forKeyedSubscript:", mdict_setObjectForKey);
    M(md, "setValue:forKey:", mdict_setObjectForKey); M(md, "removeObjectForKey:", mdict_removeObjectForKey);
    M(md, "removeAllObjects", mdict_removeAll); M(md, "addEntriesFromDictionary:", mdict_addEntries);

    cls_NSNotificationCenter = rt_define_class("NSNotificationCenter", cls_NSObject, sizeof(struct objc_object));
    CM(cls_NSNotificationCenter, "defaultCenter", nc_defaultCenter);
    M(cls_NSNotificationCenter, "addObserver:selector:name:object:", nc_addObserver);
    M(cls_NSNotificationCenter, "removeObserver:", nc_removeObserver);
    M(cls_NSNotificationCenter, "removeObserver:name:object:", nc_removeObserverName);
    M(cls_NSNotificationCenter, "postNotificationName:object:", nc_post);
    M(cls_NSNotificationCenter, "postNotificationName:object:userInfo:", nc_postUserInfo);
    cls_NSNotification = rt_define_class("NSNotification", cls_NSObject, sizeof(NoteObj));
    M(cls_NSNotification, "name", note_name); M(cls_NSNotification, "object", note_object);
    M(cls_NSNotification, "userInfo", note_userInfo);

    cls_NSUserDefaults = rt_define_class("NSUserDefaults", cls_NSObject, sizeof(DefaultsObj));
    Class u = cls_NSUserDefaults;
    CM(u, "standardUserDefaults", ud_standard); M(u, "registerDefaults:", ud_registerDefaults);
    M(u, "objectForKey:", ud_objectForKey); M(u, "boolForKey:", ud_boolForKey); M(u, "integerForKey:", ud_integerForKey);
    M(u, "floatForKey:", ud_floatForKey); M(u, "stringForKey:", ud_stringForKey); M(u, "setObject:forKey:", ud_setObject);
    M(u, "setBool:forKey:", ud_setBool); M(u, "setInteger:forKey:", ud_setInteger); M(u, "setFloat:forKey:", ud_setFloat);
    M(u, "removeObjectForKey:", ud_removeObjectForKey); M(u, "synchronize", ud_synchronize);

    cls_NSBundle = rt_define_class("NSBundle", cls_NSObject, sizeof(BundleObj));
    CM(cls_NSBundle, "mainBundle", bundle_main); M(cls_NSBundle, "resourcePath", bundle_resourcePath);
    M(cls_NSBundle, "bundlePath", bundle_resourcePath); M(cls_NSBundle, "infoDictionary", bundle_infoDictionary);
    M(cls_NSBundle, "objectForInfoDictionaryKey:", bundle_objectForInfoKey);
    M(cls_NSBundle, "pathForResource:ofType:", bundle_pathForResource);
    M(cls_NSBundle, "localizedStringForKey:value:table:", bundle_localizedString);

    cls_NSURL = rt_define_class("NSURL", cls_NSObject, sizeof(URLObj));
    CM(cls_NSURL, "URLWithString:", url_withString); M(cls_NSURL, "absoluteString", url_absoluteString);
    cls_NSRunLoop = rt_define_class("NSRunLoop", cls_NSObject, sizeof(struct objc_object));
    CM(cls_NSRunLoop, "currentRunLoop", rl_currentRunLoop); CM(cls_NSRunLoop, "mainRunLoop", rl_currentRunLoop);
    cls_NSAutoreleasePool = rt_define_class("NSAutoreleasePool", cls_NSObject, sizeof(PoolObj));
    M(cls_NSAutoreleasePool, "init", pool_init); M(cls_NSAutoreleasePool, "drain", pool_drain);
    M(cls_NSAutoreleasePool, "release", pool_drain); CM(cls_NSAutoreleasePool, "addObject:", pool_addObject);
    cls_NSProcessInfo = rt_define_class("NSProcessInfo", cls_NSObject, sizeof(struct objc_object));
    CM(cls_NSProcessInfo, "processInfo", pi_processInfo); M(cls_NSProcessInfo, "processorCount", pi_processorCount);
    M(cls_NSProcessInfo, "activeProcessorCount", pi_processorCount); M(cls_NSProcessInfo, "physicalMemory", pi_physicalMemory);
    cls_NSError = rt_define_class("NSError", cls_NSObject, sizeof(struct objc_object));
    cls_NSData = rt_define_class("NSData", cls_NSObject, sizeof(struct objc_object));
    cls_Protocol = rt_define_class("Protocol", cls_NSObject, sizeof(struct objc_object));

    cls_NSGlobalBlock = rt_define_class("__NSGlobalBlock__", cls_NSObject, sizeof(struct Block_layout));
    cls_NSStackBlock = rt_define_class("__NSStackBlock__", cls_NSObject, sizeof(struct Block_layout));
    cls_NSMallocBlock = rt_define_class("__NSMallocBlock__", cls_NSObject, sizeof(struct Block_layout));
    Class blks[] = {cls_NSGlobalBlock, cls_NSStackBlock, cls_NSMallocBlock};
    for (int i = 0; i < 3; i++) {
        rt_set_flags(blks[i], RW_NO_RC);
        M(blks[i], "copy", blk_copy); M(blks[i], "copyWithZone:", blk_copy); M(blks[i], "retain", blk_retain);
        M(blks[i], "release", blk_release); M(blks[i], "autorelease", obj_self); M(blks[i], "dealloc", obj_noop);
    }

    g_NSDefaultRunLoopMode = ns_str_new("kCFRunLoopDefaultMode");
    g_NSRunLoopCommonModes = ns_str_new("kCFRunLoopCommonModes");
}
