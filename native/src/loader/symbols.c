// Name -> address table for every import the game (or a similar iOS binary)
// needs: libSystem shims, CoreFoundation/Foundation C functions, framework
// constants, and Objective-C classes (resolved dynamically).
#define _GNU_SOURCE
#include "symbols.h"

#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <sys/time.h>
extern int __cxa_atexit(void (*)(void *), void *, void *);
void _Block_object_assign(void *, const void *, const int);
void _Block_object_dispose(const void *, const int);

#include "../fw/fw.h"
#include "../objc/objc.h"
#include "port_log.h"

// ---- darwin shims (libsystem.c / darwin_asm.S) ----
#define X(n) extern char n[];
X(darwin_open) X(darwin_fcntl) X(darwin_sprintf_chk) X(darwin_asprintf) X(darwin_sprintf) X(darwin_snprintf)
X(darwin_printf) X(darwin_fprintf) X(darwin_sscanf) X(darwin_setjmp) X(darwin_longjmp) X(darwin_sincosf_stret)
X(darwin_sincos_stret) X(darwin_NSLog)
X(darwin___error) X(darwin_stat) X(darwin_lstat) X(darwin_fstat) X(darwin_opendir) X(darwin_readdir)
X(darwin_closedir) X(darwin_fopen) X(darwin_mkdir) X(darwin_assert_rtn) X(darwin_stack_chk_guard)
X(darwin_stack_chk_fail) X(darwin_memcpy_chk) X(darwin_memmove_chk) X(darwin_memset_chk)
X(darwin_memset_pattern16) X(darwin_bzero) X(darwin_exp10) X(darwin_OSMemoryBarrier) X(darwin_rand)
X(darwin_srand) X(darwin_sysconf) X(darwin_sysctlbyname) X(darwin_dyld_register_func_for_add_image)
X(darwin_DefaultRuneLocale) X(darwin___tolower) X(darwin___toupper) X(darwin___maskrune)
X(darwin_pthread_mutexattr_init) X(darwin_pthread_mutexattr_destroy) X(darwin_pthread_mutexattr_settype)
X(darwin_pthread_mutex_init) X(darwin_pthread_mutex_destroy) X(darwin_pthread_mutex_lock)
X(darwin_pthread_mutex_trylock) X(darwin_pthread_mutex_unlock) X(darwin_pthread_cond_init)
X(darwin_pthread_cond_destroy) X(darwin_pthread_cond_signal) X(darwin_pthread_cond_broadcast)
X(darwin_pthread_cond_wait) X(darwin_pthread_cond_timedwait) X(darwin_pthread_once) X(darwin_pthread_attr_init)
X(darwin_pthread_attr_destroy) X(darwin_pthread_attr_setstacksize) X(darwin_pthread_attr_setdetachstate)
X(darwin_pthread_attr_setschedpolicy) X(darwin_pthread_attr_setschedparam) X(darwin_pthread_create)
X(darwin_pthread_join) X(darwin_pthread_self) X(darwin_pthread_setname_np) X(darwin_pthread_getschedparam)
X(darwin_pthread_setschedparam) X(darwin_sched_get_priority_min) X(darwin_sched_get_priority_max)
X(darwin_pthread_key_create) X(darwin_pthread_getspecific) X(darwin_pthread_setspecific)
X(darwin_mach_absolute_time) X(darwin_mach_timebase_info) X(darwin_mach_wait_until) X(darwin_mach_task_self_)
X(darwin_mach_host_self) X(darwin_host_page_size) X(darwin_host_statistics) X(darwin_task_info)
X(darwin_semaphore_create) X(darwin_semaphore_destroy) X(darwin_semaphore_signal) X(darwin_semaphore_signal_all)
X(darwin_semaphore_wait) X(darwin_semaphore_timedwait) X(darwin_hash_create) X(darwin_hash_search) X(darwin_exit)
X(darwin_dlsym) X(darwin_gettimeofday) X(darwin_pread) X(darwin_read) X(darwin_lseek)
// ---- frameworks ----
X(CFBundleGetMainBundle_impl) X(CFBundleGetInfoDictionary_impl) X(CFBundleCopyLocalizedString_impl)
X(CFRelease_impl) X(CFRetain_impl) X(CFStringCreateWithCStringNoCopy_impl) X(CFStringCreateWithCString_impl)
X(CFStringGetCString_impl) X(CFStringGetCStringPtr_impl) X(CFStringGetLength_impl) X(NSClassFromString_impl)
X(NSStringFromClass_impl) X(NSStringFromSelector_impl) X(NSSelectorFromString_impl)
X(NSSearchPathForDirectoriesInDomains_impl) X(NSHomeDirectory_impl) X(NSTemporaryDirectory_impl)
X(UIApplicationMain_impl)
X(AudioComponentFindNext_impl) X(AudioComponentInstanceNew_impl) X(AudioComponentInstanceDispose_impl)
X(AudioUnitSetProperty_impl) X(AudioUnitGetProperty_impl) X(AudioUnitInitialize_impl) X(AudioUnitUninitialize_impl)
X(AudioOutputUnitStart_impl) X(AudioOutputUnitStop_impl)
X(objc_empty_cache_sym) X(objc_empty_vtable_sym) X(objc_msgSendSuper)
#undef X

extern id g_NSDefaultRunLoopMode, g_NSRunLoopCommonModes;
extern uint64_t g_kCFAllocatorNull[2];
extern void *g_kCFAllocatorNullPtr;
extern double g_kCFCoreFoundationVersionNumber;
extern id g_kEAGLDrawablePropertyColorFormat, g_kEAGLDrawablePropertyRetainedBacking, g_kEAGLColorFormatRGBA8;
extern id g_AVAudioSessionCategoryAmbient, g_AVAudioSessionCategoryPlayback, g_AVAudioSessionCategorySoloAmbient;
extern id g_AVAudioSessionInterruptionNotification, g_AVAudioSessionInterruptionTypeKey;
extern id g_AVAudioSessionMediaServicesWereResetNotification, g_AVAudioSessionPortHeadphones;
extern id g_AVAudioSessionRouteChangeNotification, g_AVAudioSessionRouteChangePreviousRouteKey;
extern id g_GCControllerDidConnectNotification, g_GCControllerDidDisconnectNotification;
extern id g_GKPlayerAuthenticationDidChangeNotificationName;

// objc runtime API used by ARCLite et al. (objc_api.c)
#define Y(n) extern char n[];
Y(class_addProperty) Y(class_addProtocol) Y(class_getInstanceMethod)
Y(class_getInstanceVariable) 
Y(method_setImplementation) Y(objc_allocateClassPair) Y(objc_constructInstance)
Y(objc_copyClassNamesForImage) Y(objc_getProtocol) Y(objc_getRequiredClass)
Y(objc_initializeClassPair) Y(objc_readClassPair) Y(objc_registerClassPair) 
Y(objc_setProperty_nonatomic) Y(object_getIvar)
Y(property_copyAttributeList) Y(protocol_getMethodDescription) Y(protocol_getName) 
Y(objc_retainAutoreleasedReturnValue)
Y(objc_autoreleaseReturnValue) Y(objc_storeStrong) Y(objc_retainAutorelease) Y(objc_getProperty)
Y(objc_setProperty_nonatomic_copy) Y(objc_setProperty_atomic) Y(objc_copyStruct) Y(objc_sync_enter)
Y(objc_sync_exit) Y(objc_enumerationMutation) 
Y(objc_retainBlock)
Y(_Block_copy) Y(_Block_release) 
#undef Y

#define S(n) {"_" #n, (void *)n}
#define D(n, f) {"_" #n, (void *)f}
#define R(full, f) {full, (void *)f}

static const sym_entry g_syms[] = {
    // ---- libSystem: straight host mappings ----
    S(abort), S(access), S(acosf), S(asinf), S(atan2f), S(atanf), S(atoi), S(atol), S(atof), S(calloc), S(close),
    S(cosf), S(cos), S(sin), S(tan), S(dup), S(expf), S(exp), S(fclose), S(fmod), S(fmodf), S(fread), S(fwrite),
    S(fseek), S(ftell), S(fflush), S(fgets), S(fputs), S(free), D(gettimeofday, darwin_gettimeofday), S(ldexpf), S(log10), S(log10f),
    S(logf), S(log), D(lseek, darwin_lseek), S(malloc), S(memchr), S(memcmp), S(memcpy), S(memmove), S(memset), S(nanosleep),
    S(pow), S(powf), D(pread, darwin_pread), S(qsort), D(read, darwin_read), S(realloc), S(rename), S(sched_yield), S(sinf), S(sqrt), S(sqrtf),
    S(strcasecmp), S(strcat), S(strchr), S(strcmp), S(strcpy), S(strcspn), S(strdup), S(strlen), S(strncasecmp),
    S(strncmp), S(strncpy), S(strrchr), S(strspn), S(strstr), S(strtof), S(strtod), S(strtol), S(strtoul),
    S(strtoll), S(strtoull), S(tanf), S(time), S(unlink), S(write), S(floorf), S(ceilf), S(floor), S(ceil),
    S(roundf), S(round), S(fabsf), S(fabs), S(exp2f), S(log2f), S(atan2), S(acos), S(asin), S(atan), S(tanh),
    S(remove), S(rmdir), S(getenv), S(usleep), S(localtime), S(localtime_r), S(gmtime), S(mktime), S(strftime),
    S(clock), S(getpid), S(isatty), S(feof), S(ferror), S(fgetc), S(ungetc), S(rewind), S(posix_memalign),
    S(memmem), S(strndup), S(strerror), S(tolower), S(toupper), S(bsearch), S(abs), S(labs), S(lrintf), S(lroundf),
    S(fmaxf), S(fminf), S(hypotf), S(truncf), S(modff), S(frexpf), S(sinhf), S(coshf), S(tanhf), S(cbrtf),
    // ---- libSystem: Darwin-specific / translated ----
    D(open, darwin_open), D(fcntl, darwin_fcntl), D(__sprintf_chk, darwin_sprintf_chk),
    D(asprintf, darwin_asprintf), D(sprintf, darwin_sprintf), D(snprintf, darwin_snprintf), D(printf, darwin_printf),
    D(fprintf, darwin_fprintf), D(sscanf, darwin_sscanf), D(setjmp, darwin_setjmp), D(longjmp, darwin_longjmp),
    D(_setjmp, darwin_setjmp), D(_longjmp, darwin_longjmp), D(__sincosf_stret, darwin_sincosf_stret),
    D(__sincos_stret, darwin_sincos_stret), D(__error, darwin___error), D(stat, darwin_stat), D(lstat, darwin_lstat),
    D(fstat, darwin_fstat), D(opendir, darwin_opendir), D(readdir, darwin_readdir), D(closedir, darwin_closedir),
    D(fopen, darwin_fopen), D(mkdir, darwin_mkdir), D(__assert_rtn, darwin_assert_rtn),
    D(__stack_chk_guard, darwin_stack_chk_guard), D(__stack_chk_fail, darwin_stack_chk_fail),
    D(__memcpy_chk, darwin_memcpy_chk), D(__memmove_chk, darwin_memmove_chk), D(__memset_chk, darwin_memset_chk),
    D(memset_pattern16, darwin_memset_pattern16), D(bzero, darwin_bzero), D(__exp10, darwin_exp10),
    D(OSMemoryBarrier, darwin_OSMemoryBarrier), D(rand, darwin_rand), D(srand, darwin_srand),
    D(sysconf, darwin_sysconf), D(sysctlbyname, darwin_sysctlbyname),
    D(_dyld_register_func_for_add_image, darwin_dyld_register_func_for_add_image),
    D(_DefaultRuneLocale, darwin_DefaultRuneLocale), D(__tolower, darwin___tolower), D(__toupper, darwin___toupper),
    D(__maskrune, darwin___maskrune), D(exit, darwin_exit), D(dlsym, darwin_dlsym), D(__cxa_atexit, __cxa_atexit),
    D(pthread_mutexattr_init, darwin_pthread_mutexattr_init),
    D(pthread_mutexattr_destroy, darwin_pthread_mutexattr_destroy),
    D(pthread_mutexattr_settype, darwin_pthread_mutexattr_settype), D(pthread_mutex_init, darwin_pthread_mutex_init),
    D(pthread_mutex_destroy, darwin_pthread_mutex_destroy), D(pthread_mutex_lock, darwin_pthread_mutex_lock),
    D(pthread_mutex_trylock, darwin_pthread_mutex_trylock), D(pthread_mutex_unlock, darwin_pthread_mutex_unlock),
    D(pthread_cond_init, darwin_pthread_cond_init), D(pthread_cond_destroy, darwin_pthread_cond_destroy),
    D(pthread_cond_signal, darwin_pthread_cond_signal), D(pthread_cond_broadcast, darwin_pthread_cond_broadcast),
    D(pthread_cond_wait, darwin_pthread_cond_wait), D(pthread_cond_timedwait, darwin_pthread_cond_timedwait),
    D(pthread_once, darwin_pthread_once), D(pthread_attr_init, darwin_pthread_attr_init),
    D(pthread_attr_destroy, darwin_pthread_attr_destroy),
    D(pthread_attr_setstacksize, darwin_pthread_attr_setstacksize),
    D(pthread_attr_setdetachstate, darwin_pthread_attr_setdetachstate),
    D(pthread_attr_setschedpolicy, darwin_pthread_attr_setschedpolicy),
    D(pthread_attr_setschedparam, darwin_pthread_attr_setschedparam), D(pthread_create, darwin_pthread_create),
    D(pthread_join, darwin_pthread_join), D(pthread_self, darwin_pthread_self),
    D(pthread_setname_np, darwin_pthread_setname_np), D(pthread_getschedparam, darwin_pthread_getschedparam),
    D(pthread_setschedparam, darwin_pthread_setschedparam),
    D(sched_get_priority_min, darwin_sched_get_priority_min),
    D(sched_get_priority_max, darwin_sched_get_priority_max), D(pthread_key_create, darwin_pthread_key_create),
    D(pthread_getspecific, darwin_pthread_getspecific), D(pthread_setspecific, darwin_pthread_setspecific),
    D(mach_absolute_time, darwin_mach_absolute_time), D(mach_timebase_info, darwin_mach_timebase_info),
    D(mach_wait_until, darwin_mach_wait_until), D(mach_task_self_, darwin_mach_task_self_),
    D(mach_host_self, darwin_mach_host_self), D(host_page_size, darwin_host_page_size),
    D(host_statistics, darwin_host_statistics), D(task_info, darwin_task_info),
    D(semaphore_create, darwin_semaphore_create), D(semaphore_destroy, darwin_semaphore_destroy),
    D(semaphore_signal, darwin_semaphore_signal), D(semaphore_signal_all, darwin_semaphore_signal_all),
    D(semaphore_wait, darwin_semaphore_wait), D(semaphore_timedwait, darwin_semaphore_timedwait),
    D(hash_create, darwin_hash_create), D(hash_search, darwin_hash_search),
    // ---- zlib ----
    S(inflate), S(inflateEnd), S(inflateInit2_), S(inflateReset), S(inflateInit_), S(deflate), S(deflateEnd),
    S(deflateInit_), S(deflateInit2_), S(crc32), S(adler32), S(uncompress), S(compress),
    // ---- CoreFoundation / Foundation C API ----
    D(CFBundleGetMainBundle, CFBundleGetMainBundle_impl), D(CFBundleGetInfoDictionary, CFBundleGetInfoDictionary_impl),
    D(CFBundleCopyLocalizedString, CFBundleCopyLocalizedString_impl), D(CFRelease, CFRelease_impl),
    D(CFRetain, CFRetain_impl), D(CFStringCreateWithCStringNoCopy, CFStringCreateWithCStringNoCopy_impl),
    D(CFStringCreateWithCString, CFStringCreateWithCString_impl), D(CFStringGetCString, CFStringGetCString_impl),
    D(CFStringGetCStringPtr, CFStringGetCStringPtr_impl), D(CFStringGetLength, CFStringGetLength_impl),
    D(NSClassFromString, NSClassFromString_impl), D(NSStringFromClass, NSStringFromClass_impl),
    D(NSStringFromSelector, NSStringFromSelector_impl), D(NSSelectorFromString, NSSelectorFromString_impl),
    D(NSSearchPathForDirectoriesInDomains, NSSearchPathForDirectoriesInDomains_impl),
    D(NSHomeDirectory, NSHomeDirectory_impl), D(NSTemporaryDirectory, NSTemporaryDirectory_impl),
    D(NSLog, darwin_NSLog), D(UIApplicationMain, UIApplicationMain_impl),
    // ---- data constants (address of the variable holding the value) ----
    D(NSDefaultRunLoopMode, &g_NSDefaultRunLoopMode), D(NSRunLoopCommonModes, &g_NSRunLoopCommonModes),
    D(kCFAllocatorNull, &g_kCFAllocatorNullPtr), D(kCFCoreFoundationVersionNumber, &g_kCFCoreFoundationVersionNumber),
    D(kEAGLDrawablePropertyColorFormat, &g_kEAGLDrawablePropertyColorFormat),
    D(kEAGLDrawablePropertyRetainedBacking, &g_kEAGLDrawablePropertyRetainedBacking),
    D(kEAGLColorFormatRGBA8, &g_kEAGLColorFormatRGBA8),
    D(AVAudioSessionCategoryAmbient, &g_AVAudioSessionCategoryAmbient),
    D(AVAudioSessionCategoryPlayback, &g_AVAudioSessionCategoryPlayback),
    D(AVAudioSessionCategorySoloAmbient, &g_AVAudioSessionCategorySoloAmbient),
    D(AVAudioSessionInterruptionNotification, &g_AVAudioSessionInterruptionNotification),
    D(AVAudioSessionInterruptionTypeKey, &g_AVAudioSessionInterruptionTypeKey),
    D(AVAudioSessionMediaServicesWereResetNotification, &g_AVAudioSessionMediaServicesWereResetNotification),
    D(AVAudioSessionPortHeadphones, &g_AVAudioSessionPortHeadphones),
    D(AVAudioSessionRouteChangeNotification, &g_AVAudioSessionRouteChangeNotification),
    D(AVAudioSessionRouteChangePreviousRouteKey, &g_AVAudioSessionRouteChangePreviousRouteKey),
    D(GCControllerDidConnectNotification, &g_GCControllerDidConnectNotification),
    D(GCControllerDidDisconnectNotification, &g_GCControllerDidDisconnectNotification),
    D(GKPlayerAuthenticationDidChangeNotificationName, &g_GKPlayerAuthenticationDidChangeNotificationName),
    // ---- AudioToolbox ----
    D(AudioComponentFindNext, AudioComponentFindNext_impl), D(AudioComponentInstanceNew, AudioComponentInstanceNew_impl),
    D(AudioComponentInstanceDispose, AudioComponentInstanceDispose_impl),
    D(AudioUnitSetProperty, AudioUnitSetProperty_impl), D(AudioUnitGetProperty, AudioUnitGetProperty_impl),
    D(AudioUnitInitialize, AudioUnitInitialize_impl), D(AudioUnitUninitialize, AudioUnitUninitialize_impl),
    D(AudioOutputUnitStart, AudioOutputUnitStart_impl), D(AudioOutputUnitStop, AudioOutputUnitStop_impl),
    // ---- libobjc ----
    D(_objc_empty_cache, objc_empty_cache_sym), D(_objc_empty_vtable, objc_empty_vtable_sym),
    S(objc_msgSend), S(objc_msgSendSuper2), S(objc_msgSendSuper), S(class_addMethod), S(class_addProperty),
    S(class_addProtocol), S(class_getInstanceMethod), S(class_getInstanceVariable), S(class_getName),
    S(class_getSuperclass), S(class_isMetaClass), S(class_replaceMethod), S(method_setImplementation), S(objc_alloc),
    S(objc_allocateClassPair), S(objc_autorelease), S(objc_constructInstance), S(objc_copyClassNamesForImage),
    S(objc_getClass), S(objc_getMetaClass), S(objc_getProtocol), S(objc_getRequiredClass), S(objc_initializeClassPair),
    S(objc_lookUpClass), S(objc_readClassPair), S(objc_registerClassPair), S(objc_release), S(objc_retain),
    S(objc_setProperty_nonatomic), S(object_getClass), S(object_getIndexedIvars), S(object_getIvar),
    S(property_copyAttributeList), S(protocol_getMethodDescription), S(protocol_getName), S(sel_getUid),
    S(sel_registerName), S(objc_autoreleasePoolPush), S(objc_autoreleasePoolPop),
    S(objc_retainAutoreleasedReturnValue), S(objc_autoreleaseReturnValue), S(objc_storeStrong),
    S(objc_retainAutorelease), S(objc_getProperty), S(objc_setProperty_nonatomic_copy), S(objc_setProperty_atomic),
    S(objc_copyStruct), S(objc_sync_enter), S(objc_sync_exit), S(objc_enumerationMutation),
    S(class_getMethodImplementation), S(class_respondsToSelector), S(class_createInstance), S(object_dispose),
    S(sel_getName), S(class_getInstanceSize), S(objc_retainBlock), S(_Block_copy), S(_Block_release),
    S(_Block_object_assign), S(_Block_object_dispose),
    {0, 0}};

// ---- hash map over both tables ----
static struct { const char *k; void *v; } *g_map;
static unsigned g_mask;
static unsigned h(const char *s) {
    unsigned v = 2166136261u;
    while (*s) { v ^= (unsigned char)*s++; v *= 16777619u; }
    return v;
}
static void put(const char *k, void *v) {
    unsigned i = h(k) & g_mask;
    while (g_map[i].k) {
        if (!strcmp(g_map[i].k, k)) return;
        i = (i + 1) & g_mask;
    }
    g_map[i].k = k;
    g_map[i].v = v;
}
void symbols_init(void) {
    g_mask = 2047;
    g_map = calloc(g_mask + 1, sizeof(*g_map));
    for (const sym_entry *e = g_syms; e->name; e++) put(e->name, e->addr);
    for (const sym_entry *e = g_cxx_syms; e->name; e++) put(e->name, e->addr);
}

extern Class cls_NSConstantString, cls_NSGlobalBlock, cls_NSStackBlock, cls_NSMallocBlock;

#ifdef FTL_GUARD_ALLOC
void *guard_malloc(size_t), *guard_calloc(size_t, size_t), *guard_realloc(void *, size_t);
void guard_free(void *);
#endif

void *port_resolve_symbol(const char *sym, const char *dylib) {
    (void)dylib;
#ifdef FTL_GUARD_ALLOC
    static int guard = -1;
    if (guard < 0) guard = getenv("FTL_GUARD") != NULL;
    if (guard) {
        if (!strcmp(sym, "_malloc") || !strcmp(sym, "__Znwm") || !strcmp(sym, "__Znam")) return (void *)guard_malloc;
        if (!strcmp(sym, "_free") || !strcmp(sym, "__ZdlPv") || !strcmp(sym, "__ZdaPv")) return (void *)guard_free;
        if (!strcmp(sym, "_calloc")) return (void *)guard_calloc;
        if (!strcmp(sym, "_realloc")) return (void *)guard_realloc;
    }
#endif
    if (!strncmp(sym, "_OBJC_CLASS_$_", 14)) {
        Class c = objc_getClass(sym + 14);
        if (!c) LOGW("symbols: missing class %s", sym + 14);
        return c;
    }
    if (!strncmp(sym, "_OBJC_METACLASS_$_", 18)) {
        Class c = objc_getClass(sym + 18);
        return c ? c->isa : NULL;
    }
    if (!strcmp(sym, "___CFConstantStringClassReference")) return cls_NSConstantString;
    if (!strcmp(sym, "__NSConcreteGlobalBlock")) return cls_NSGlobalBlock;
    if (!strcmp(sym, "__NSConcreteStackBlock")) return cls_NSStackBlock;
    if (!strcmp(sym, "__NSConcreteMallocBlock")) return cls_NSMallocBlock;
    if (!strcmp(sym, "dyld_stub_binder")) return NULL;  // never used: lazy binds are resolved eagerly
    unsigned i = h(sym) & g_mask;
    while (g_map[i].k) {
        if (!strcmp(g_map[i].k, sym)) return g_map[i].v;
        i = (i + 1) & g_mask;
    }
    return NULL;
}
