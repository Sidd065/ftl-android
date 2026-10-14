// UIKit replacement: UIApplicationMain + main run loop, a minimal view tree,
// touches, UIScreen/UIDevice, UIAlertView and a UITextField that maps onto
// the platform's native text-input dialog.
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../platform/platform.h"
#include "fw.h"
#include "port_log.h"

Class cls_UIApplication, cls_UIScreen, cls_UIDevice, cls_UIResponder, cls_UIView, cls_UIWindow;
Class cls_UIViewController, cls_UILabel, cls_UITextField, cls_UIColor, cls_UIAlertView;
Class cls_UITouch, cls_UIEvent, cls_UIFont;

#define M(cls, sel, fn) rt_method(cls, sel, (void *)(fn))
#define CM(cls, sel, fn) rt_cmethod(cls, sel, (void *)(fn))

fw_display_config g_display = {2048, 1536, 2.0, 60, 2048, 1536};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ---------------------------------------------------------------------------
// main-thread work queue
typedef struct mq_item {
    void (*fn)(void *);
    void *ud;
    id target;
    SEL sel;
    id arg;
    int *done;
    struct mq_item *next;
} mq_item;
static mq_item *g_mq_head, *g_mq_tail;
static pthread_mutex_t g_mq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mq_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_mq_done = PTHREAD_COND_INITIALIZER;
static pthread_t g_main_thread;
static int g_main_thread_set;

bool fw_is_main_thread(void) { return g_main_thread_set && pthread_equal(pthread_self(), g_main_thread); }

static void mq_push(mq_item *it, bool wait) {
    int done = 0;
    if (wait) it->done = &done;
    pthread_mutex_lock(&g_mq_lock);
    if (g_mq_tail) g_mq_tail->next = it; else g_mq_head = it;
    g_mq_tail = it;
    pthread_cond_broadcast(&g_mq_cond);
    if (wait)
        while (!done) pthread_cond_wait(&g_mq_done, &g_mq_lock);
    pthread_mutex_unlock(&g_mq_lock);
    plat_wake_main();
}

void fw_perform_on_main(id target, SEL sel, id arg, bool wait) {
    if (wait && fw_is_main_thread()) {
        MSG(void, id)(target, sel, arg);
        return;
    }
    mq_item *it = calloc(1, sizeof(*it));
    it->target = target ? MSG(id)(target, SEL_("retain")) : nil;
    it->sel = sel;
    it->arg = arg ? MSG(id)(arg, SEL_("retain")) : nil;
    mq_push(it, wait);
}

void fw_run_on_main(void (*fn)(void *), void *ud, bool wait) {
    if (fw_is_main_thread()) {
        if (wait) { fn(ud); return; }
    }
    mq_item *it = calloc(1, sizeof(*it));
    it->fn = fn;
    it->ud = ud;
    mq_push(it, wait && !fw_is_main_thread());
}

static void mq_drain(void) {
    for (;;) {
        pthread_mutex_lock(&g_mq_lock);
        mq_item *it = g_mq_head;
        if (it) {
            g_mq_head = it->next;
            if (!g_mq_head) g_mq_tail = NULL;
        }
        pthread_mutex_unlock(&g_mq_lock);
        if (!it) return;
        void *pool = objc_autoreleasePoolPush();
        if (it->fn) it->fn(it->ud);
        else {
            MSG(void, id)(it->target, it->sel, it->arg);
            if (it->target) MSG(void)(it->target, SEL_("release"));
            if (it->arg) MSG(void)(it->arg, SEL_("release"));
        }
        objc_autoreleasePoolPop(pool);
        if (it->done) {
            pthread_mutex_lock(&g_mq_lock);
            *it->done = 1;
            pthread_cond_broadcast(&g_mq_done);
            pthread_mutex_unlock(&g_mq_lock);
        }
        free(it);
    }
}

// ---------------------------------------------------------------------------
// display links (registered by quartz.c)
typedef struct dl_entry { id link; struct dl_entry *next; } dl_entry;
static dl_entry *g_links;
static double g_next_vsync;
extern void quartz_fire_display_link(id link, double ts, double duration);
extern bool quartz_link_active(id link);
void uikit_register_display_link(id link) {
    dl_entry *e = calloc(1, sizeof(*e));
    e->link = link;
    e->next = g_links;
    g_links = e;
}
void uikit_unregister_display_link(id link) {
    for (dl_entry **pp = &g_links; *pp; pp = &(*pp)->next)
        if ((*pp)->link == link) { dl_entry *d = *pp; *pp = d->next; free(d); return; }
}

static volatile int g_app_active = 1;

void fw_runloop_iterate(double max_wait_s) {
    mq_drain();
    plat_pump_events();
    mq_drain();
    double t = now_s();
    double period = 1.0 / (g_display.refresh_hz > 0 ? g_display.refresh_hz : 60);
    if (g_app_active && t >= g_next_vsync) {
        if (g_next_vsync == 0 || t - g_next_vsync > 0.25) g_next_vsync = t;
        g_next_vsync += period;
        for (dl_entry *e = g_links; e; e = e->next) {
            if (!quartz_link_active(e->link)) continue;
            void *pool = objc_autoreleasePoolPush();
            quartz_fire_display_link(e->link, t, period);
            objc_autoreleasePoolPop(pool);
        }
    }
    double wait = g_next_vsync - now_s();
    if (!g_app_active) wait = 0.05;
    if (wait > max_wait_s) wait = max_wait_s;
    if (wait > 0) {
        pthread_mutex_lock(&g_mq_lock);
        if (!g_mq_head) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            long ns = ts.tv_nsec + (long)(wait * 1e9);
            ts.tv_sec += ns / 1000000000L;
            ts.tv_nsec = ns % 1000000000L;
            pthread_cond_timedwait(&g_mq_cond, &g_mq_lock, &ts);
        }
        pthread_mutex_unlock(&g_mq_lock);
    }
}

// ---------------------------------------------------------------------------
// UIResponder / UIView
typedef struct {
    Class isa;
    CGRect frame;
    CGRect bounds;
    id layer;
    id subviews;  // NSMutableArray
    id superview;
    id background;
    double content_scale;
    bool hidden, multitouch, opaque, user_interaction;
    id text;        // UILabel / UITextField
    id delegate;    // UITextField
    int alignment, border, keyboard;
    id text_color;
    id root_vc;     // UIWindow
} ViewObj;

static id view_initWithFrame(id self, SEL _c, CGRect f) {
    (void)_c;
    ViewObj *v = (ViewObj *)self;
    v->frame = f;
    v->bounds = CGRectMake(0, 0, f.size.width, f.size.height);
    v->content_scale = 1.0;
    v->user_interaction = true;
    v->subviews = MSG(id)(MSG(id)((id)cls_NSMutableArray, SEL_("alloc")), SEL_("init"));
    return self;
}
static id view_init(id self, SEL _c) { (void)_c; return view_initWithFrame(self, NULL, CGRectMake(0, 0, 0, 0)); }
static void view_dealloc(id self, SEL _c) {
    ViewObj *v = (ViewObj *)self;
    if (v->layer) MSG(void)(v->layer, SEL_("release"));
    if (v->subviews) MSG(void)(v->subviews, SEL_("release"));
    if (v->text) MSG(void)(v->text, SEL_("release"));
    rt_dealloc_object(self);
    (void)_c;
}
static Class view_layerClass(Class self, SEL _c) { (void)self; (void)_c; return cls_CALayer; }
static id view_layer(id self, SEL _c) {
    (void)_c;
    ViewObj *v = (ViewObj *)self;
    if (!v->layer) {
        Class lc = MSG(Class)((id)self->isa, SEL_("layerClass"));
        v->layer = MSG(id)(MSG(id)((id)lc, SEL_("alloc")), SEL_("init"));
        MSG(void, CGRect)(v->layer, SEL_("setFrame:"), v->frame);
        MSG(void, double)(v->layer, SEL_("setContentsScale:"), v->content_scale);
    }
    return v->layer;
}
static CGRect view_frame(id self, SEL _c) { (void)_c; return ((ViewObj *)self)->frame; }
static CGRect view_bounds(id self, SEL _c) { (void)_c; return ((ViewObj *)self)->bounds; }
static void view_setFrame(id self, SEL _c, CGRect f) {
    (void)_c;
    ViewObj *v = (ViewObj *)self;
    v->frame = f;
    v->bounds.size = f.size;
    if (v->layer) MSG(void, CGRect)(v->layer, SEL_("setFrame:"), f);
}
static void view_setBounds(id self, SEL _c, CGRect b) { (void)_c; ((ViewObj *)self)->bounds = b; }
static CGPoint view_center(id self, SEL _c) {
    (void)_c;
    ViewObj *v = (ViewObj *)self;
    return (CGPoint){v->frame.origin.x + v->frame.size.width / 2, v->frame.origin.y + v->frame.size.height / 2};
}
static double view_contentScaleFactor(id self, SEL _c) { (void)_c; return ((ViewObj *)self)->content_scale; }
static void view_setContentScaleFactor(id self, SEL _c, double s) {
    (void)_c;
    ViewObj *v = (ViewObj *)self;
    v->content_scale = s;
    if (v->layer) MSG(void, double)(v->layer, SEL_("setContentsScale:"), s);
}
static void view_setBool(id self, SEL _c, BOOL b) { (void)self; (void)_c; (void)b; }
static void view_setMultipleTouchEnabled(id self, SEL _c, BOOL b) { (void)_c; ((ViewObj *)self)->multitouch = b; }
static void view_setHidden(id self, SEL _c, BOOL b) { (void)_c; ((ViewObj *)self)->hidden = b; }
static BOOL view_isHidden(id self, SEL _c) { (void)_c; return ((ViewObj *)self)->hidden; }
static void view_setUserInteractionEnabled(id self, SEL _c, BOOL b) { (void)_c; ((ViewObj *)self)->user_interaction = b; }
static void view_setBackgroundColor(id self, SEL _c, id c) {
    (void)_c;
    ViewObj *v = (ViewObj *)self;
    if (c) MSG(id)(c, SEL_("retain"));
    if (v->background) MSG(void)(v->background, SEL_("release"));
    v->background = c;
}
static void view_addSubview(id self, SEL _c, id sub) {
    (void)_c;
    if (!sub) return;
    ViewObj *v = (ViewObj *)self;
    ViewObj *s = (ViewObj *)sub;
    if (s->superview == self) return;
    if (s->superview) MSG(void)(sub, SEL_("removeFromSuperview"));
    ns_marray_add(v->subviews, sub);
    s->superview = self;
    if (sub->isa == cls_UITextField || objc_isKindOfClass(sub, cls_UITextField)) {
        // nothing: keyboard opens on becomeFirstResponder
    }
}
static void view_removeFromSuperview(id self, SEL _c) {
    (void)_c;
    ViewObj *s = (ViewObj *)self;
    if (!s->superview) return;
    id sup = s->superview;
    s->superview = nil;
    MSG(void, id)(((ViewObj *)sup)->subviews, SEL_("removeObject:"), self);
}
static id view_superview(id self, SEL _c) { (void)_c; return ((ViewObj *)self)->superview; }
static id view_subviews(id self, SEL _c) { (void)_c; return ((ViewObj *)self)->subviews; }
static id view_window(id self, SEL _c) {
    (void)_c;
    id v = self;
    while (v && !objc_isKindOfClass(v, cls_UIWindow)) v = ((ViewObj *)v)->superview;
    return v;
}
static void view_noop1(id self, SEL _c, id a) { (void)self; (void)_c; (void)a; }
static void view_noop(id self, SEL _c) { (void)self; (void)_c; }
static void view_touches(id self, SEL _c, id touches, id event) { (void)self; (void)_c; (void)touches; (void)event; }

// UIWindow
static id g_key_window;
static void win_setRootViewController(id self, SEL _c, id vc) {
    (void)_c;
    ViewObj *w = (ViewObj *)self;
    if (vc) MSG(id)(vc, SEL_("retain"));
    w->root_vc = vc;
    if (vc) {
        id v = MSG(id)(vc, SEL_("view"));
        if (v) {
            MSG(void, CGRect)(v, SEL_("setFrame:"), w->bounds);
            view_addSubview(self, NULL, v);
        }
    }
}
static id win_rootViewController(id self, SEL _c) { (void)_c; return ((ViewObj *)self)->root_vc; }
static void win_makeKeyAndVisible(id self, SEL _c) { (void)_c; g_key_window = self; LOGI("UIWindow makeKeyAndVisible"); }
static void win_makeKeyWindow(id self, SEL _c) { (void)_c; g_key_window = self; }

// UIViewController
typedef struct { Class isa; id view; } VCObj;
static id vc_initWithNibName(id self, SEL _c, id nib, id bundle) { (void)_c; (void)nib; (void)bundle; return self; }
static id vc_init(id self, SEL _c) { (void)_c; return self; }
static id vc_view(id self, SEL _c) {
    (void)_c;
    VCObj *vc = (VCObj *)self;
    if (!vc->view) {
        MSG(void)(self, SEL_("loadView"));
        if (vc->view) MSG(void)(self, SEL_("viewDidLoad"));
    }
    return vc->view;
}
static void vc_setView(id self, SEL _c, id v) {
    (void)_c;
    VCObj *vc = (VCObj *)self;
    if (v) MSG(id)(v, SEL_("retain"));
    if (vc->view) MSG(void)(vc->view, SEL_("release"));
    vc->view = v;
}
static void vc_loadView(id self, SEL _c) {
    (void)_c;
    id v = MSG(id, CGRect)(MSG(id)((id)cls_UIView, SEL_("alloc")), SEL_("initWithFrame:"),
                          CGRectMake(0, 0, g_display.render_w / g_display.scale, g_display.render_h / g_display.scale));
    vc_setView(self, NULL, v);
    MSG(void)(v, SEL_("release"));
}
static BOOL vc_isViewLoaded(id self, SEL _c) { (void)_c; return ((VCObj *)self)->view != nil; }
static NSUInteger vc_supportedOrientations(id self, SEL _c) { (void)self; (void)_c; return 0x18; }

// UILabel / UITextField
static void label_setText(id self, SEL _c, id t) {
    (void)_c;
    ViewObj *v = (ViewObj *)self;
    id c = t ? MSG(id)(t, SEL_("copy")) : nil;
    if (v->text) MSG(void)(v->text, SEL_("release"));
    v->text = c;
}
static id label_text(id self, SEL _c) { (void)_c; return ((ViewObj *)self)->text ? ((ViewObj *)self)->text : ns_str(""); }
static void label_setTextAlignment(id self, SEL _c, NSInteger a) { (void)_c; ((ViewObj *)self)->alignment = (int)a; }
static void label_setTextColor(id self, SEL _c, id c) { (void)_c; ((ViewObj *)self)->text_color = c; }
static void tf_setDelegate(id self, SEL _c, id d) { (void)_c; ((ViewObj *)self)->delegate = d; }
static id tf_delegate(id self, SEL _c) { (void)_c; return ((ViewObj *)self)->delegate; }
static void tf_setBorderStyle(id self, SEL _c, NSInteger b) { (void)_c; ((ViewObj *)self)->border = (int)b; }
static void tf_setKeyboardType(id self, SEL _c, NSInteger k) { (void)_c; ((ViewObj *)self)->keyboard = (int)k; }

static id g_editing_field;  // UITextField currently being edited
static BOOL tf_becomeFirstResponder(id self, SEL _c) {
    (void)_c;
    ViewObj *tf = (ViewObj *)self;
    if (g_editing_field) return NO;
    g_editing_field = MSG(id)(self, SEL_("retain"));
    // A UILabel sibling (if any) carries the prompt.
    const char *prompt = NULL;
    if (tf->superview) {
        id subs = ((ViewObj *)tf->superview)->subviews;
        for (size_t i = 0; i < ns_array_count(subs); i++) {
            id s = ns_array_at(subs, i);
            if (s != self && s->isa == cls_UILabel && ((ViewObj *)s)->text) prompt = ns_utf8(((ViewObj *)s)->text);
        }
    }
    const char *initial = tf->text ? ns_utf8(tf->text) : "";
    LOGI("text input requested (prompt '%s', initial '%s')", prompt ? prompt : "", initial);
    plat_request_text_input(prompt, initial);
    return YES;
}
static BOOL tf_resignFirstResponder(id self, SEL _c) {
    (void)_c;
    if (g_editing_field != self) return YES;
    g_editing_field = nil;
    id d = ((ViewObj *)self)->delegate;
    if (d && class_respondsToSelector(object_getClass(d), SEL_("textFieldDidEndEditing:")))
        MSG(void, id)(d, SEL_("textFieldDidEndEditing:"), self);
    plat_cancel_text_input();
    MSG(void)(self, SEL_("release"));
    return YES;
}
static BOOL tf_isFirstResponder(id self, SEL _c) { (void)_c; return g_editing_field == self; }

typedef struct { char *text; bool accepted; } text_result;
static void text_result_main(void *ud) {
    text_result *r = ud;
    id tf = g_editing_field;
    if (tf) {
        if (r->accepted && r->text) label_setText(tf, NULL, ns_str(r->text));
        id d = ((ViewObj *)tf)->delegate;
        BOOL handled = NO;
        if (d && class_respondsToSelector(object_getClass(d), SEL_("textFieldShouldReturn:"))) {
            MSG(BOOL, id)(d, SEL_("textFieldShouldReturn:"), tf);
            handled = YES;
        }
        if (g_editing_field == tf) tf_resignFirstResponder(tf, NULL);
        (void)handled;
    }
    free(r->text);
    free(r);
}
void fw_text_input_result(const char *text, bool accepted) {
    text_result *r = calloc(1, sizeof(*r));
    r->text = text ? strdup(text) : NULL;
    r->accepted = accepted;
    fw_run_on_main(text_result_main, r, false);
}

// UIColor
typedef struct { Class isa; double r, g, b, a; } ColorObj;
static id color_make(double r, double g, double b, double a) {
    ColorObj *c = (ColorObj *)class_createInstance(cls_UIColor, 0);
    c->r = r; c->g = g; c->b = b; c->a = a;
    return rt_autorelease((id)c);
}
static id color_white(Class s, SEL c) { (void)s; (void)c; return color_make(1, 1, 1, 1); }
static id color_black(Class s, SEL c) { (void)s; (void)c; return color_make(0, 0, 0, 1); }
static id color_clear(Class s, SEL c) { (void)s; (void)c; return color_make(0, 0, 0, 0); }
static id color_withWhite(Class s, SEL c, double w, double a) { (void)s; (void)c; return color_make(w, w, w, a); }
static id color_withRGBA(Class s, SEL c, double r, double g, double b, double a) { (void)s; (void)c; return color_make(r, g, b, a); }

// ---------------------------------------------------------------------------
// UIScreen / UIDevice
static id g_main_screen;
static id screen_main(Class self, SEL _c) {
    (void)_c;
    if (!g_main_screen) g_main_screen = class_createInstance(self, 0);
    return g_main_screen;
}
static CGRect screen_bounds(id self, SEL _c) {
    (void)self; (void)_c;
    return CGRectMake(0, 0, g_display.render_w / g_display.scale, g_display.render_h / g_display.scale);
}
static CGRect screen_nativeBounds(id self, SEL _c) {
    (void)self; (void)_c;
    return CGRectMake(0, 0, g_display.render_h, g_display.render_w);  // portrait-oriented
}
static double screen_scale(id self, SEL _c) { (void)self; (void)_c; return g_display.scale; }
static NSInteger screen_maxFPS(id self, SEL _c) { (void)self; (void)_c; return 60; }
static id screen_screens(Class self, SEL _c) { id s = screen_main(self, _c); return ns_array(&s, 1); }

static id g_device;
static id dev_current(Class self, SEL _c) {
    (void)_c;
    if (!g_device) g_device = class_createInstance(self, 0);
    return g_device;
}
static id dev_systemVersion(id self, SEL _c) { (void)self; (void)_c; return ns_str("15.0"); }
static id dev_model(id self, SEL _c) { (void)self; (void)_c; return ns_str("iPad"); }
static id dev_name(id self, SEL _c) { (void)self; (void)_c; return ns_str("Android"); }
static id dev_systemName(id self, SEL _c) { (void)self; (void)_c; return ns_str("iPadOS"); }
static NSInteger dev_idiom(id self, SEL _c) { (void)self; (void)_c; return 1; }  // UIUserInterfaceIdiomPad
static NSInteger dev_orientation(id self, SEL _c) { (void)self; (void)_c; return 3; }  // LandscapeLeft
static void dev_noop(id self, SEL _c) { (void)self; (void)_c; }

// ---------------------------------------------------------------------------
// UIApplication
typedef struct { Class isa; id delegate; bool idle_disabled; } AppObj;
static id g_app;
static id app_shared(Class self, SEL _c) { (void)self; (void)_c; return g_app; }
static id app_delegate(id self, SEL _c) { (void)_c; return ((AppObj *)self)->delegate; }
static void app_setDelegate(id self, SEL _c, id d) { (void)_c; ((AppObj *)self)->delegate = d; }
static void app_setIdleTimerDisabled(id self, SEL _c, BOOL b) {
    (void)_c;
    ((AppObj *)self)->idle_disabled = b;
    plat_keep_screen_on(true);  // a game should never dim mid-battle
}
static BOOL app_isIdleTimerDisabled(id self, SEL _c) { (void)_c; return ((AppObj *)self)->idle_disabled; }
static void app_setStatusBarHidden(id self, SEL _c, BOOL h, NSInteger anim) { (void)self; (void)_c; (void)h; (void)anim; }
static CGRect app_statusBarFrame(id self, SEL _c) { (void)self; (void)_c; return CGRectMake(0, 0, 0, 0); }
static BOOL app_openURL(id self, SEL _c, id url) {
    (void)self; (void)_c;
    id s = url ? MSG(id)(url, SEL_("absoluteString")) : nil;
    if (s) plat_open_url(ns_utf8(s));
    return YES;
}
static BOOL app_canOpenURL(id self, SEL _c, id url) { (void)self; (void)_c; (void)url; return YES; }
static id app_keyWindow(id self, SEL _c) { (void)self; (void)_c; return g_key_window; }
static id app_windows(id self, SEL _c) { (void)self; (void)_c; return g_key_window ? ns_array(&g_key_window, 1) : ns_array(NULL, 0); }
static NSInteger app_applicationState(id self, SEL _c) { (void)self; (void)_c; return g_app_active ? 0 : 2; }

// ---------------------------------------------------------------------------
// UIAlertView
typedef struct { Class isa; id title, message, delegate; id buttons; NSInteger cancel_index; } AlertObj;
static id g_showing_alert;
// variadic otherButtonTitles: on Apple arm64 the extra titles live on the stack.
id alert_initWithTitle_impl(id self, SEL _c, id title, id msg, id delegate, id cancel, id other, id *stack) {
    (void)_c;
    AlertObj *a = (AlertObj *)self;
    a->title = title ? MSG(id)(title, SEL_("copy")) : nil;
    a->message = msg ? MSG(id)(msg, SEL_("copy")) : nil;
    a->delegate = delegate;
    a->buttons = MSG(id)(MSG(id)((id)cls_NSMutableArray, SEL_("alloc")), SEL_("init"));
    a->cancel_index = -1;
    if (cancel) { a->cancel_index = 0; ns_marray_add(a->buttons, cancel); }
    if (other) {
        ns_marray_add(a->buttons, other);
        for (int i = 0; stack && stack[i] && i < 8; i++) ns_marray_add(a->buttons, stack[i]);
    }
    return self;
}
extern id alert_initWithTitle_tramp(id, SEL, id, id, id, id, id);
__asm__(".text\n.globl alert_initWithTitle_tramp\n.p2align 2\nalert_initWithTitle_tramp:\n  mov x7, sp\n  b alert_initWithTitle_impl\n");

static NSInteger alert_cancelButtonIndex(id self, SEL _c) { (void)_c; return ((AlertObj *)self)->cancel_index; }
static NSInteger alert_addButtonWithTitle(id self, SEL _c, id t) {
    (void)_c;
    AlertObj *a = (AlertObj *)self;
    ns_marray_add(a->buttons, t);
    return (NSInteger)ns_array_count(a->buttons) - 1;
}
static void alert_show(id self, SEL _c) {
    (void)_c;
    AlertObj *a = (AlertObj *)self;
    const char *btns[8];
    int n = 0;
    for (size_t i = 0; i < ns_array_count(a->buttons) && n < 8; i++) btns[n++] = ns_utf8(ns_array_at(a->buttons, i));
    LOGI("UIAlertView: %s / %s", a->title ? ns_utf8(a->title) : "", a->message ? ns_utf8(a->message) : "");
    g_showing_alert = MSG(id)(self, SEL_("retain"));
    plat_show_alert(a->title ? ns_utf8(a->title) : "", a->message ? ns_utf8(a->message) : "", btns, n);
}
static void alert_result_main(void *ud) {
    long idx = (long)(intptr_t)ud;
    id al = g_showing_alert;
    g_showing_alert = nil;
    if (!al) return;
    AlertObj *a = (AlertObj *)al;
    if (a->delegate) {
        if (class_respondsToSelector(object_getClass(a->delegate), SEL_("alertView:clickedButtonAtIndex:")))
            MSG(void, id, NSInteger)(a->delegate, SEL_("alertView:clickedButtonAtIndex:"), al, idx);
        if (class_respondsToSelector(object_getClass(a->delegate), SEL_("alertView:didDismissWithButtonIndex:")))
            MSG(void, id, NSInteger)(a->delegate, SEL_("alertView:didDismissWithButtonIndex:"), al, idx);
    }
    MSG(void)(al, SEL_("release"));
}
void fw_alert_result(int button) { fw_run_on_main(alert_result_main, (void *)(intptr_t)button, false); }

// ---------------------------------------------------------------------------
// UITouch / UIEvent
typedef struct { Class isa; CGPoint loc, prev; double ts; NSInteger phase; id view; NSUInteger taps; } TouchObj;
#define MAX_TOUCHES 16
static struct { long id; id touch; } g_touches[MAX_TOUCHES];
static id g_event;
static CGPoint touch_locationInView(id self, SEL _c, id view) { (void)_c; (void)view; return ((TouchObj *)self)->loc; }
static CGPoint touch_prevLocationInView(id self, SEL _c, id view) { (void)_c; (void)view; return ((TouchObj *)self)->prev; }
static double touch_timestamp(id self, SEL _c) { (void)_c; return ((TouchObj *)self)->ts; }
static NSInteger touch_phase(id self, SEL _c) { (void)_c; return ((TouchObj *)self)->phase; }
static id touch_view(id self, SEL _c) { (void)_c; return ((TouchObj *)self)->view; }
static NSUInteger touch_tapCount(id self, SEL _c) { (void)_c; return ((TouchObj *)self)->taps; }
static double touch_majorRadius(id self, SEL _c) { (void)self; (void)_c; return 20.0; }
static double event_timestamp(id self, SEL _c) { (void)self; (void)_c; return now_s(); }
static id event_allTouches(id self, SEL _c) {
    (void)self; (void)_c;
    id list[MAX_TOUCHES];
    size_t n = 0;
    for (int i = 0; i < MAX_TOUCHES; i++) if (g_touches[i].touch) list[n++] = g_touches[i].touch;
    return rt_autorelease(ns_set_new(list, n));
}
static NSInteger event_type(id self, SEL _c) { (void)self; (void)_c; return 0; }

static id touch_target_view(void) {
    if (!g_key_window) return nil;
    id vc = ((ViewObj *)g_key_window)->root_vc;
    if (vc && ((VCObj *)vc)->view) return ((VCObj *)vc)->view;
    id subs = ((ViewObj *)g_key_window)->subviews;
    return ns_array_count(subs) ? ns_array_at(subs, 0) : nil;
}

void fw_dispatch_touch(int phase, long tid, double x_px, double y_px, double ts) {
    id view = touch_target_view();
    if (!view) return;
    int slot = -1;
    for (int i = 0; i < MAX_TOUCHES; i++) if (g_touches[i].touch && g_touches[i].id == tid) slot = i;
    if (phase == TOUCH_BEGAN) {
        if (slot >= 0) return;
        for (int i = 0; i < MAX_TOUCHES && slot < 0; i++) if (!g_touches[i].touch) slot = i;
        if (slot < 0) return;
        TouchObj *t = (TouchObj *)class_createInstance(cls_UITouch, 0);
        t->view = view;
        t->taps = 1;
        t->loc = t->prev = (CGPoint){x_px / g_display.scale, y_px / g_display.scale};
        g_touches[slot].touch = (id)t;
        g_touches[slot].id = tid;
    } else if (slot < 0) {
        return;
    }
    TouchObj *t = (TouchObj *)g_touches[slot].touch;
    t->prev = t->loc;
    t->loc = (CGPoint){x_px / g_display.scale, y_px / g_display.scale};
    t->ts = ts;
    t->phase = phase;
    if (!g_event) g_event = class_createInstance(cls_UIEvent, 0);
    void *pool = objc_autoreleasePoolPush();
    id one = (id)t;
    id set = rt_autorelease(ns_set_new(&one, 1));
    const char *sel = phase == TOUCH_BEGAN ? "touchesBegan:withEvent:"
                      : phase == TOUCH_MOVED ? "touchesMoved:withEvent:"
                      : phase == TOUCH_ENDED ? "touchesEnded:withEvent:"
                                             : "touchesCancelled:withEvent:";
    MSG(void, id, id)(view, sel_registerName(sel), set, g_event);
    objc_autoreleasePoolPop(pool);
    if (phase == TOUCH_ENDED || phase == TOUCH_CANCELLED) {
        MSG(void)(g_touches[slot].touch, SEL_("release"));
        g_touches[slot].touch = nil;
    }
}

// ---------------------------------------------------------------------------
// lifecycle
static void call_delegate(const char *selname) {
    if (!g_app) return;
    id d = ((AppObj *)g_app)->delegate;
    SEL s = sel_registerName(selname);
    if (d && class_respondsToSelector(object_getClass(d), s)) {
        LOGI("app delegate: %s", selname);
        void *pool = objc_autoreleasePoolPush();
        MSG(void, id)(d, s, g_app);
        objc_autoreleasePoolPop(pool);
    }
}
static void resign_main(void *u) { (void)u; if (g_app_active) { call_delegate("applicationWillResignActive:"); g_app_active = 0; } }
static void active_main(void *u) { (void)u; if (!g_app_active) { g_app_active = 1; call_delegate("applicationDidBecomeActive:"); } }
static void terminate_main(void *u) { (void)u; call_delegate("applicationWillTerminate:"); }
static void memwarn_main(void *u) { (void)u; call_delegate("applicationDidReceiveMemoryWarning:"); }
void fw_app_will_resign_active(void) { fw_run_on_main(resign_main, NULL, false); }
void fw_app_did_become_active(void) { fw_run_on_main(active_main, NULL, false); }
void fw_app_will_terminate(void) { fw_run_on_main(terminate_main, NULL, false); }
void fw_app_memory_warning(void) { fw_run_on_main(memwarn_main, NULL, false); }

int UIApplicationMain_impl(int argc, char **argv, id principal, id delegate_name) {
    (void)argc; (void)argv;
    g_main_thread = pthread_self();
    g_main_thread_set = 1;
    void *pool = objc_autoreleasePoolPush();
    Class appcls = principal ? objc_getClass(ns_utf8(principal)) : cls_UIApplication;
    if (!appcls) appcls = cls_UIApplication;
    g_app = class_createInstance(appcls, 0);
    Class dcls = delegate_name ? objc_getClass(ns_utf8(delegate_name)) : Nil;
    LOGI("UIApplicationMain: app=%s delegate=%s", class_getName(appcls), dcls ? class_getName(dcls) : "(none)");
    id delegate = dcls ? MSG(id)(MSG(id)((id)dcls, SEL_("alloc")), SEL_("init")) : nil;
    ((AppObj *)g_app)->delegate = delegate;
    plat_main_thread_started();
    if (delegate) {
        SEL s = SEL_("application:didFinishLaunchingWithOptions:");
        if (class_respondsToSelector(object_getClass(delegate), s)) MSG(BOOL, id, id)(delegate, s, g_app, nil);
        else call_delegate("applicationDidFinishLaunching:");
    }
    call_delegate("applicationDidBecomeActive:");
    objc_autoreleasePoolPop(pool);
    for (;;) {
        pool = objc_autoreleasePoolPush();
        fw_runloop_iterate(0.1);
        objc_autoreleasePoolPop(pool);
    }
    return 0;
}

// ---------------------------------------------------------------------------
void fw_configure_display(int sw, int sh, int hz) {
    g_display.surface_w = sw;
    g_display.surface_h = sh;
    if (hz > 0) g_display.refresh_hz = hz;
}

void fw_uikit_init(void) {
    cls_UIResponder = rt_define_class("UIResponder", cls_NSObject, sizeof(struct objc_object));
    M(cls_UIResponder, "becomeFirstResponder", view_setBool);
    M(cls_UIResponder, "resignFirstResponder", view_setBool);
    M(cls_UIResponder, "touchesBegan:withEvent:", view_touches);
    M(cls_UIResponder, "touchesMoved:withEvent:", view_touches);
    M(cls_UIResponder, "touchesEnded:withEvent:", view_touches);
    M(cls_UIResponder, "touchesCancelled:withEvent:", view_touches);

    cls_UIView = rt_define_class("UIView", cls_UIResponder, sizeof(ViewObj));
    Class v = cls_UIView;
    M(v, "initWithFrame:", view_initWithFrame); M(v, "init", view_init); M(v, "dealloc", view_dealloc);
    CM(v, "layerClass", view_layerClass); M(v, "layer", view_layer); M(v, "frame", view_frame);
    M(v, "bounds", view_bounds); M(v, "setFrame:", view_setFrame); M(v, "setBounds:", view_setBounds);
    M(v, "center", view_center); M(v, "contentScaleFactor", view_contentScaleFactor);
    M(v, "setContentScaleFactor:", view_setContentScaleFactor); M(v, "setMultipleTouchEnabled:", view_setMultipleTouchEnabled);
    M(v, "setExclusiveTouch:", view_setBool); M(v, "setOpaque:", view_setBool); M(v, "setHidden:", view_setHidden);
    M(v, "isHidden", view_isHidden); M(v, "setUserInteractionEnabled:", view_setUserInteractionEnabled);
    M(v, "setBackgroundColor:", view_setBackgroundColor); M(v, "addSubview:", view_addSubview);
    M(v, "removeFromSuperview", view_removeFromSuperview); M(v, "superview", view_superview);
    M(v, "subviews", view_subviews); M(v, "window", view_window); M(v, "bringSubviewToFront:", view_noop1);
    M(v, "sendSubviewToBack:", view_noop1); M(v, "setNeedsLayout", view_noop); M(v, "layoutIfNeeded", view_noop);
    M(v, "setNeedsDisplay", view_noop); M(v, "setAutoresizingMask:", view_noop1); M(v, "setAlpha:", view_noop1);

    cls_UIWindow = rt_define_class("UIWindow", cls_UIView, sizeof(ViewObj));
    M(cls_UIWindow, "setRootViewController:", win_setRootViewController);
    M(cls_UIWindow, "rootViewController", win_rootViewController);
    M(cls_UIWindow, "makeKeyAndVisible", win_makeKeyAndVisible); M(cls_UIWindow, "makeKeyWindow", win_makeKeyWindow);

    cls_UIViewController = rt_define_class("UIViewController", cls_UIResponder, sizeof(VCObj));
    Class c = cls_UIViewController;
    M(c, "initWithNibName:bundle:", vc_initWithNibName); M(c, "init", vc_init); M(c, "view", vc_view);
    M(c, "setView:", vc_setView); M(c, "loadView", vc_loadView); M(c, "viewDidLoad", view_noop);
    M(c, "isViewLoaded", vc_isViewLoaded); M(c, "supportedInterfaceOrientations", vc_supportedOrientations);
    M(c, "didReceiveMemoryWarning", view_noop); M(c, "setNeedsStatusBarAppearanceUpdate", view_noop);

    cls_UILabel = rt_define_class("UILabel", cls_UIView, sizeof(ViewObj));
    M(cls_UILabel, "setText:", label_setText); M(cls_UILabel, "text", label_text);
    M(cls_UILabel, "setTextAlignment:", label_setTextAlignment); M(cls_UILabel, "setTextColor:", label_setTextColor);
    M(cls_UILabel, "setFont:", view_noop1);
    cls_UITextField = rt_define_class("UITextField", cls_UIView, sizeof(ViewObj));
    Class t = cls_UITextField;
    M(t, "setText:", label_setText); M(t, "text", label_text); M(t, "setDelegate:", tf_setDelegate);
    M(t, "delegate", tf_delegate); M(t, "setBorderStyle:", tf_setBorderStyle); M(t, "setKeyboardType:", tf_setKeyboardType);
    M(t, "setTextAlignment:", label_setTextAlignment); M(t, "setTextColor:", label_setTextColor);
    M(t, "becomeFirstResponder", tf_becomeFirstResponder); M(t, "resignFirstResponder", tf_resignFirstResponder);
    M(t, "isFirstResponder", tf_isFirstResponder); M(t, "setAutocorrectionType:", view_noop1);
    M(t, "setAutocapitalizationType:", view_noop1); M(t, "setReturnKeyType:", view_noop1); M(t, "setFont:", view_noop1);
    M(t, "setPlaceholder:", view_noop1); M(t, "setClearButtonMode:", view_noop1);

    cls_UIColor = rt_define_class("UIColor", cls_NSObject, sizeof(ColorObj));
    CM(cls_UIColor, "whiteColor", color_white); CM(cls_UIColor, "blackColor", color_black);
    CM(cls_UIColor, "clearColor", color_clear); CM(cls_UIColor, "colorWithWhite:alpha:", color_withWhite);
    CM(cls_UIColor, "colorWithRed:green:blue:alpha:", color_withRGBA);
    cls_UIFont = rt_define_class("UIFont", cls_NSObject, sizeof(struct objc_object));

    cls_UIScreen = rt_define_class("UIScreen", cls_NSObject, sizeof(struct objc_object));
    CM(cls_UIScreen, "mainScreen", screen_main); CM(cls_UIScreen, "screens", screen_screens);
    M(cls_UIScreen, "bounds", screen_bounds); M(cls_UIScreen, "applicationFrame", screen_bounds);
    M(cls_UIScreen, "nativeBounds", screen_nativeBounds); M(cls_UIScreen, "scale", screen_scale);
    M(cls_UIScreen, "nativeScale", screen_scale); M(cls_UIScreen, "maximumFramesPerSecond", screen_maxFPS);

    cls_UIDevice = rt_define_class("UIDevice", cls_NSObject, sizeof(struct objc_object));
    CM(cls_UIDevice, "currentDevice", dev_current); M(cls_UIDevice, "systemVersion", dev_systemVersion);
    M(cls_UIDevice, "model", dev_model); M(cls_UIDevice, "localizedModel", dev_model); M(cls_UIDevice, "name", dev_name);
    M(cls_UIDevice, "systemName", dev_systemName); M(cls_UIDevice, "userInterfaceIdiom", dev_idiom);
    M(cls_UIDevice, "orientation", dev_orientation);
    M(cls_UIDevice, "beginGeneratingDeviceOrientationNotifications", dev_noop);

    cls_UIApplication = rt_define_class("UIApplication", cls_UIResponder, sizeof(AppObj));
    Class a = cls_UIApplication;
    CM(a, "sharedApplication", app_shared); M(a, "delegate", app_delegate); M(a, "setDelegate:", app_setDelegate);
    M(a, "setIdleTimerDisabled:", app_setIdleTimerDisabled); M(a, "isIdleTimerDisabled", app_isIdleTimerDisabled);
    M(a, "setStatusBarHidden:withAnimation:", app_setStatusBarHidden); M(a, "setStatusBarHidden:", view_setBool);
    M(a, "statusBarFrame", app_statusBarFrame); M(a, "openURL:", app_openURL); M(a, "canOpenURL:", app_canOpenURL);
    M(a, "keyWindow", app_keyWindow); M(a, "windows", app_windows); M(a, "applicationState", app_applicationState);
    M(a, "beginReceivingRemoteControlEvents", view_noop); M(a, "endReceivingRemoteControlEvents", view_noop);
    M(a, "registerForRemoteNotifications", view_noop);

    cls_UIAlertView = rt_define_class("UIAlertView", cls_UIView, sizeof(AlertObj));
    M(cls_UIAlertView, "initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles:", alert_initWithTitle_tramp);
    M(cls_UIAlertView, "show", alert_show); M(cls_UIAlertView, "cancelButtonIndex", alert_cancelButtonIndex);
    M(cls_UIAlertView, "addButtonWithTitle:", alert_addButtonWithTitle);

    cls_UITouch = rt_define_class("UITouch", cls_NSObject, sizeof(TouchObj));
    M(cls_UITouch, "locationInView:", touch_locationInView);
    M(cls_UITouch, "previousLocationInView:", touch_prevLocationInView);
    M(cls_UITouch, "timestamp", touch_timestamp); M(cls_UITouch, "phase", touch_phase);
    M(cls_UITouch, "view", touch_view); M(cls_UITouch, "tapCount", touch_tapCount);
    M(cls_UITouch, "majorRadius", touch_majorRadius);
    cls_UIEvent = rt_define_class("UIEvent", cls_NSObject, sizeof(struct objc_object));
    M(cls_UIEvent, "timestamp", event_timestamp); M(cls_UIEvent, "allTouches", event_allTouches);
    M(cls_UIEvent, "type", event_type);
}
