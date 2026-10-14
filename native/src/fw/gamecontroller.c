// GameController framework stubs. The iOS build listens for MFi controllers,
// but the game itself never acts on controller events, so no controller is
// ever reported here; gamepads are handled by the port's UX layer instead
// (an on-screen cursor that touches the screen, see ux/ux.c).
#include "fw.h"

#define CM(cls, sel, fn) rt_cmethod(cls, sel, (void *)(fn))

Class cls_GCController;
id g_GCControllerDidConnectNotification, g_GCControllerDidDisconnectNotification;

static id gc_controllers(Class s, SEL c) { (void)s; (void)c; return ns_array(NULL, 0); }
static void gc_noop(id s, SEL c) { (void)s; (void)c; }
static void gc_noop1(id s, SEL c, id a) { (void)s; (void)c; (void)a; }

void fw_gc_init(void) {
    cls_GCController = rt_define_class("GCController", cls_NSObject, sizeof(struct objc_object));
    CM(cls_GCController, "controllers", gc_controllers);
    CM(cls_GCController, "startWirelessControllerDiscoveryWithCompletionHandler:", gc_noop1);
    CM(cls_GCController, "stopWirelessControllerDiscovery", gc_noop);
    g_GCControllerDidConnectNotification = ns_str_new("GCControllerDidConnectNotification");
    g_GCControllerDidDisconnectNotification = ns_str_new("GCControllerDidDisconnectNotification");
}
