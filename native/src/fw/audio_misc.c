// AudioToolbox (RemoteIO AudioUnit) + AVAudioSession on the platform audio
// callback, and stubs for GameKit (Game Center disabled).
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"
#include "fw.h"
#include "port_log.h"

Class cls_AVAudioSession, cls_AVAudioSessionRouteDescription, cls_AVAudioSessionPortDescription;
Class cls_GKLocalPlayer, cls_GKAchievement;

#define M(cls, sel, fn) rt_method(cls, sel, (void *)(fn))
#define CM(cls, sel, fn) rt_cmethod(cls, sel, (void *)(fn))

// ---------------------------------------------------------------------------
// AudioUnit emulation
typedef struct {
    double mSampleRate;
    uint32_t mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame,
        mBitsPerChannel, mReserved;
} ASBD;
typedef int32_t (*AURenderCallback)(void *refcon, uint32_t *flags, const void *ts, uint32_t bus, uint32_t frames,
                                    void *iodata);
typedef struct {
    uint32_t mNumberChannels;
    uint32_t mDataByteSize;
    void *mData;
} AudioBuffer;
typedef struct {
    uint32_t mNumberBuffers;
    AudioBuffer mBuffers[1];
} AudioBufferList;
typedef struct {
    double mSampleTime;
    uint64_t mHostTime;
    double mRateScalar;
    uint64_t mWordClockTime;
    uint8_t mSMPTETime[24];
    uint32_t mFlags;
    uint32_t mReserved;
} AudioTimeStamp;

typedef struct {
    ASBD fmt;
    AURenderCallback cb;
    void *refcon;
    bool initialized, running;
    double sample_time;
} AudioUnitObj;
static AudioUnitObj g_au;
static int g_component_token = 0x41554e54;

void *AudioComponentFindNext_impl(void *in, const void *desc) {
    (void)in; (void)desc;
    return &g_component_token;
}
int32_t AudioComponentInstanceNew_impl(void *comp, void **out) {
    (void)comp;
    memset(&g_au, 0, sizeof g_au);
    g_au.fmt.mSampleRate = 44100;
    g_au.fmt.mChannelsPerFrame = 2;
    g_au.fmt.mBitsPerChannel = 16;
    *out = &g_au;
    return 0;
}
int32_t AudioComponentInstanceDispose_impl(void *u) { (void)u; plat_audio_pause(true); return 0; }
int32_t AudioUnitSetProperty_impl(void *u, uint32_t prop, uint32_t scope, uint32_t elem, const void *data, uint32_t size) {
    (void)u; (void)scope; (void)elem;
    if (prop == 8 && size >= sizeof(ASBD)) {  // kAudioUnitProperty_StreamFormat
        memcpy(&g_au.fmt, data, sizeof(ASBD));
        LOGI("AudioUnit format: %.0f Hz, %u ch, %u bits, flags %#x", g_au.fmt.mSampleRate, g_au.fmt.mChannelsPerFrame,
             g_au.fmt.mBitsPerChannel, g_au.fmt.mFormatFlags);
    } else if (prop == 23 && size >= 16) {  // kAudioUnitProperty_SetRenderCallback
        memcpy(&g_au.cb, data, 8);
        memcpy(&g_au.refcon, (const uint8_t *)data + 8, 8);
    } else {
        LOGV("AudioUnitSetProperty(%u) ignored", prop);
    }
    return 0;
}
int32_t AudioUnitGetProperty_impl(void *u, uint32_t prop, uint32_t scope, uint32_t elem, void *data, uint32_t *size) {
    (void)u; (void)scope; (void)elem;
    if (prop == 8 && *size >= sizeof(ASBD)) { memcpy(data, &g_au.fmt, sizeof(ASBD)); return 0; }
    return -10879;  // kAudioUnitErr_InvalidProperty
}
int32_t AudioUnitInitialize_impl(void *u) { (void)u; g_au.initialized = true; return 0; }
int32_t AudioUnitUninitialize_impl(void *u) { (void)u; g_au.initialized = false; return 0; }

static void audio_cb(void *ud, int16_t *out, int frames) {
    (void)ud;
    if (!g_au.cb || !g_au.running) { memset(out, 0, (size_t)frames * 4); return; }
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = 2;
    abl.mBuffers[0].mDataByteSize = (uint32_t)frames * 4;
    abl.mBuffers[0].mData = out;
    AudioTimeStamp ts = {0};
    ts.mSampleTime = g_au.sample_time;
    ts.mFlags = 1;
    uint32_t flags = 0;
    g_au.cb(g_au.refcon, &flags, &ts, 0, (uint32_t)frames, &abl);
    g_au.sample_time += frames;
}

int32_t AudioOutputUnitStart_impl(void *u) {
    (void)u;
    if (g_au.fmt.mChannelsPerFrame != 2 || g_au.fmt.mBitsPerChannel != 16)
        LOGW("AudioUnit: unexpected format, audio may be garbled");
    if (!g_au.running) {
        g_au.running = true;
        if (!plat_audio_open((int)g_au.fmt.mSampleRate, 2, audio_cb, NULL)) LOGE("audio device failed to open");
        plat_audio_pause(false);
    }
    return 0;
}
int32_t AudioOutputUnitStop_impl(void *u) {
    (void)u;
    g_au.running = false;
    plat_audio_pause(true);
    return 0;
}

// AudioSession C API (older API): accept everything.
int32_t AudioSession_ok_impl(void) { return 0; }

// ---------------------------------------------------------------------------
// AVAudioSession
static id g_session, g_route, g_port;
id g_AVAudioSessionCategoryAmbient, g_AVAudioSessionCategoryPlayback, g_AVAudioSessionCategorySoloAmbient;
id g_AVAudioSessionInterruptionNotification, g_AVAudioSessionInterruptionTypeKey;
id g_AVAudioSessionMediaServicesWereResetNotification, g_AVAudioSessionPortHeadphones, g_AVAudioSessionPortBuiltInSpeaker;
id g_AVAudioSessionRouteChangeNotification, g_AVAudioSessionRouteChangePreviousRouteKey;

static id sess_shared(Class self, SEL _c) {
    (void)_c;
    if (!g_session) g_session = class_createInstance(self, 0);
    return g_session;
}
static BOOL sess_yes3(id s, SEL c, id a, NSUInteger b, id *err) { (void)s; (void)c; (void)a; (void)b; if (err) *err = nil; return YES; }
static BOOL sess_yes2(id s, SEL c, id a, id *err) { (void)s; (void)c; (void)a; if (err) *err = nil; return YES; }
static BOOL sess_setActive(id s, SEL c, BOOL a, id *err) { (void)s; (void)c; (void)a; if (err) *err = nil; return YES; }
static BOOL sess_setDouble(id s, SEL c, double v, id *err) { (void)s; (void)c; (void)v; if (err) *err = nil; return YES; }
static double sess_ioBufferDuration(id s, SEL c) { (void)s; (void)c; return 0.01; }
static double sess_outputLatency(id s, SEL c) { (void)s; (void)c; return 0.02; }
static double sess_sampleRate(id s, SEL c) { (void)s; (void)c; int r = plat_audio_native_rate(); return r > 0 ? r : 48000; }
static BOOL sess_no(id s, SEL c) { (void)s; (void)c; return NO; }
static id sess_currentRoute(id s, SEL c) {
    (void)s; (void)c;
    if (!g_route) {
        g_route = class_createInstance(cls_AVAudioSessionRouteDescription, 0);
        g_port = class_createInstance(cls_AVAudioSessionPortDescription, 0);
    }
    return g_route;
}
static id route_outputs(id s, SEL c) { (void)s; (void)c; return ns_array(&g_port, 1); }
static id port_portType(id s, SEL c) { (void)s; (void)c; return g_AVAudioSessionPortBuiltInSpeaker; }

static id empty_array(id s, SEL c) { (void)s; (void)c; return ns_array(NULL, 0); }

// GameKit: present but never authenticated (NSClassFromString hides it).
static id gk_localPlayer(Class self, SEL _c) {
    (void)_c;
    static id p;
    if (!p) p = class_createInstance(self, 0);
    return p;
}
static BOOL gk_no(id s, SEL c) { (void)s; (void)c; return NO; }
static void gk_noop1(id s, SEL c, id a) { (void)s; (void)c; (void)a; }
static id gk_nil(id s, SEL c) { (void)s; (void)c; return nil; }
static id gk_initWithIdentifier(id s, SEL c, id ident) { (void)c; (void)ident; return s; }
id g_GKPlayerAuthenticationDidChangeNotificationName;

void fw_audio_init(void) {
    cls_AVAudioSession = rt_define_class("AVAudioSession", cls_NSObject, sizeof(struct objc_object));
    Class s = cls_AVAudioSession;
    CM(s, "sharedInstance", sess_shared);
    M(s, "setCategory:withOptions:error:", sess_yes3); M(s, "setCategory:error:", sess_yes2);
    M(s, "setMode:error:", sess_yes2); M(s, "setActive:error:", sess_setActive);
    M(s, "setActive:withOptions:error:", sess_yes3);
    M(s, "setPreferredIOBufferDuration:error:", sess_setDouble); M(s, "setPreferredSampleRate:error:", sess_setDouble);
    M(s, "IOBufferDuration", sess_ioBufferDuration); M(s, "outputLatency", sess_outputLatency);
    M(s, "sampleRate", sess_sampleRate); M(s, "preferredSampleRate", sess_sampleRate);
    M(s, "isOtherAudioPlaying", sess_no); M(s, "secondaryAudioShouldBeSilencedHint", sess_no);
    M(s, "currentRoute", sess_currentRoute);
    cls_AVAudioSessionRouteDescription = rt_define_class("AVAudioSessionRouteDescription", cls_NSObject, sizeof(struct objc_object));
    M(cls_AVAudioSessionRouteDescription, "outputs", route_outputs);
    M(cls_AVAudioSessionRouteDescription, "inputs", empty_array);
    cls_AVAudioSessionPortDescription = rt_define_class("AVAudioSessionPortDescription", cls_NSObject, sizeof(struct objc_object));
    M(cls_AVAudioSessionPortDescription, "portType", port_portType);

    g_AVAudioSessionCategoryAmbient = ns_str_new("AVAudioSessionCategoryAmbient");
    g_AVAudioSessionCategoryPlayback = ns_str_new("AVAudioSessionCategoryPlayback");
    g_AVAudioSessionCategorySoloAmbient = ns_str_new("AVAudioSessionCategorySoloAmbient");
    g_AVAudioSessionInterruptionNotification = ns_str_new("AVAudioSessionInterruptionNotification");
    g_AVAudioSessionInterruptionTypeKey = ns_str_new("AVAudioSessionInterruptionTypeKey");
    g_AVAudioSessionMediaServicesWereResetNotification = ns_str_new("AVAudioSessionMediaServicesWereResetNotification");
    g_AVAudioSessionPortHeadphones = ns_str_new("Headphones");
    g_AVAudioSessionPortBuiltInSpeaker = ns_str_new("Speaker");
    g_AVAudioSessionRouteChangeNotification = ns_str_new("AVAudioSessionRouteChangeNotification");
    g_AVAudioSessionRouteChangePreviousRouteKey = ns_str_new("AVAudioSessionRouteChangePreviousRouteKey");
}

void fw_misc_init(void) {
    fw_gc_init();

    cls_GKLocalPlayer = rt_define_class("GKLocalPlayer", cls_NSObject, sizeof(struct objc_object));
    CM(cls_GKLocalPlayer, "localPlayer", gk_localPlayer);
    M(cls_GKLocalPlayer, "isAuthenticated", gk_no); M(cls_GKLocalPlayer, "authenticateWithCompletionHandler:", gk_noop1);
    M(cls_GKLocalPlayer, "setAuthenticateHandler:", gk_noop1); M(cls_GKLocalPlayer, "playerID", gk_nil);
    cls_GKAchievement = rt_define_class("GKAchievement", cls_NSObject, sizeof(struct objc_object) + 16);
    M(cls_GKAchievement, "initWithIdentifier:", gk_initWithIdentifier);
    M(cls_GKAchievement, "reportAchievementWithCompletionHandler:", gk_noop1);
    CM(cls_GKAchievement, "loadAchievementsWithCompletionHandler:", gk_noop1);
    CM(cls_GKAchievement, "resetAchievementsWithCompletionHandler:", gk_noop1);
    g_GKPlayerAuthenticationDidChangeNotificationName = ns_str_new("GKPlayerAuthenticationDidChangeNotificationName");
}
