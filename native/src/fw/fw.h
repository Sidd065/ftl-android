// Shared declarations for our replacement iOS frameworks.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../objc/objc.h"

typedef double CGFloat;
typedef struct { CGFloat x, y; } CGPoint;
typedef struct { CGFloat width, height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
static inline CGRect CGRectMake(CGFloat x, CGFloat y, CGFloat w, CGFloat h) { return (CGRect){{x, y}, {w, h}}; }

typedef long NSInteger;
typedef unsigned long NSUInteger;

// ---- class objects (exported to the game through the symbol table) ----------
extern Class cls_NSObject, cls_NSString, cls_NSMutableString, cls_NSConstantString, cls_NSNumber;
extern Class cls_NSArray, cls_NSMutableArray, cls_NSDictionary, cls_NSMutableDictionary, cls_NSSet;
extern Class cls_NSEnumerator, cls_NSBundle, cls_NSNotificationCenter, cls_NSNotification, cls_NSUserDefaults;
extern Class cls_NSURL, cls_NSRunLoop, cls_NSAutoreleasePool, cls_NSData, cls_NSError, cls_NSProcessInfo;
extern Class cls_NSGlobalBlock, cls_NSStackBlock, cls_NSMallocBlock, cls_Protocol;
extern Class cls_UIApplication, cls_UIScreen, cls_UIDevice, cls_UIResponder, cls_UIView, cls_UIWindow;
extern Class cls_UIViewController, cls_UILabel, cls_UITextField, cls_UIColor, cls_UIAlertView;
extern Class cls_UITouch, cls_UIEvent, cls_UIFont;
extern Class cls_CALayer, cls_CAEAGLLayer, cls_CADisplayLink, cls_EAGLContext, cls_EAGLSharegroup;
extern Class cls_AVAudioSession, cls_AVAudioSessionRouteDescription, cls_AVAudioSessionPortDescription;
extern Class cls_GCController, cls_GKLocalPlayer, cls_GKAchievement;

void fw_foundation_init(void);
void fw_uikit_init(void);
void fw_quartz_init(void);
void fw_eagl_init(void);
void fw_audio_init(void);
void fw_misc_init(void);
void fw_eagl_constants(void);

// ---- NSString helpers -------------------------------------------------------
// All string objects share the CFConstantString layout.
typedef struct {
    Class isa;
    uint64_t flags;
    const char *str;  // UTF-8, NUL-terminated
    long length;      // bytes
} StrObj;
#define STR_OWNED (1ull << 40)

id ns_str(const char *utf8);              // autoreleased
id ns_str_new(const char *utf8);          // +1
id ns_str_n_new(const char *utf8, size_t n);
const char *ns_utf8(id str);              // NULL if not a string
bool ns_is_string(id o);
bool ns_str_eq(id a, id b);
bool ns_equal(id a, id b);
unsigned long ns_hash(id o);

// ---- collections --------------------------------------------------------------
id ns_array_new(id *objs, size_t n);      // +1, retains elements
id ns_array(id *objs, size_t n);          // autoreleased
size_t ns_array_count(id arr);
id ns_array_at(id arr, size_t i);
void ns_marray_add(id arr, id obj);
id ns_set_new(id *objs, size_t n);
id ns_dict_new(void);                     // mutable, +1
void ns_dict_set(id dict, id key, id val);
id ns_dict_get(id dict, id key);
id ns_number_int(long long v);            // autoreleased
id ns_number_double(double v);
long long ns_number_ll(id n);

// ---- notifications ------------------------------------------------------------
void ns_post_notification(id name, id object, id userinfo);

// ---- main-thread machinery (UIKit run loop) -----------------------------------
bool fw_is_main_thread(void);
void fw_perform_on_main(id target, SEL sel, id arg, bool wait);
void fw_run_on_main(void (*fn)(void *), void *ud, bool wait);
void fw_runloop_iterate(double max_wait_s);

// ---- display / screen configuration --------------------------------------------
typedef struct {
    int render_w, render_h;   // pixel size the game renders at
    double scale;             // UIScreen scale (points -> pixels)
    int refresh_hz;
    int surface_w, surface_h; // real window/surface size in pixels
} fw_display_config;
extern fw_display_config g_display;
void fw_configure_display(int surface_w, int surface_h, int refresh_hz);

// ---- paths -----------------------------------------------------------------------
extern char g_bundle_path[512];   // contains ftl.dat
extern char g_docs_path[512];     // saves / Application Support
extern char g_cache_path[512];

// ---- blocks ------------------------------------------------------------------
struct Block_layout {
    void *isa;
    volatile int32_t flags;
    int32_t reserved;
    void (*invoke)(void *, ...);
    struct Block_descriptor {
        unsigned long reserved;
        unsigned long size;
        void (*copy)(void *dst, const void *src);
        void (*dispose)(const void *);
    } *descriptor;
};
void *fw_block_copy(const void *blk);
void fw_block_release(const void *blk);

void fw_gc_init(void);  // GameController stubs

// ---- touches -----------------------------------------------------------------
enum { TOUCH_BEGAN = 0, TOUCH_MOVED = 1, TOUCH_STATIONARY = 2, TOUCH_ENDED = 3, TOUCH_CANCELLED = 4 };
void fw_dispatch_touch(int phase, long id, double x_px, double y_px, double timestamp);

// text input (UITextField)
void fw_text_input_result(const char *text, bool accepted);

// lifecycle
void fw_app_will_resign_active(void);
void fw_app_did_become_active(void);
void fw_app_will_terminate(void);
void fw_app_memory_warning(void);

// EAGL composite hook (eagl.c)
void eagl_present_hook(void);
