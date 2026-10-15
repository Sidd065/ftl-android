#define _GNU_SOURCE
// Boots the iOS executable: runtime + frameworks init, Mach-O load/bind,
// Objective-C registration, static initializers, then the game's main().
#include "port_main.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "darwin/libsystem.h"
#include "fw/fw.h"
#include "hyperspace.h"
#include "loader/macho.h"
#include "loader/symbols.h"
#include "objc/objc.h"
#include "patches.h"
#include "port_log.h"

int port_verbose = 0;
static mo_image g_img;
static uintptr_t g_text_start, g_text_end, g_unwind_info, g_eh_frame;
static size_t g_unwind_info_size, g_eh_frame_size;

void port_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOGE("FATAL: %s", buf);
    port_report_fatal(buf);
    abort();
}

// Called by our patched libunwind to find unwind info for Mach-O frames.
int __ftl_find_macho_unwind(uintptr_t pc, uintptr_t *mh, uintptr_t *textlen, uintptr_t *cu, size_t *culen,
                            uintptr_t *eh, size_t *ehlen) {
    if (pc < g_text_start || pc >= g_text_end) return 0;
    *mh = (uintptr_t)g_img.base;
    *textlen = g_text_end - g_text_start;
    *cu = g_unwind_info;
    *culen = g_unwind_info_size;
    *eh = g_eh_frame;
    *ehlen = g_eh_frame_size;
    return 1;
}

static const char *g_mods_index;
const char *port_mods_index(void) { return g_mods_index; }

mo_image *port_image(void) { return &g_img; }
uintptr_t port_image_slide(void) { return (uintptr_t)g_img.slide; }

int port_run_game(const port_config *cfg) {
    strncpy(g_bundle_path, cfg->bundle_dir, sizeof g_bundle_path - 1);
    strncpy(g_docs_path, cfg->docs_dir, sizeof g_docs_path - 1);
    strncpy(g_cache_path, cfg->cache_dir, sizeof g_cache_path - 1);
    mkdir(g_docs_path, 0755);
    mkdir(g_cache_path, 0755);
    port_verbose = cfg->verbose;
    g_mods_index = cfg->mods_index;
    if (cfg->hw_machine) darwin_set_hw_machine(cfg->hw_machine);

    darwin_libsystem_init();
    rt_init();
    fw_foundation_init();
    fw_uikit_init();
    fw_quartz_init();
    fw_eagl_init();
    fw_eagl_constants();
    fw_audio_init();
    fw_misc_init();
    symbols_init();

    if (mo_load(cfg->binary_path, &g_img) != 0) { port_report_fatal("Could not load the FTL executable."); return 1; }
    for (int i = 0; i < g_img.nsegs; i++) {
        if (!strcmp(g_img.segs[i].name, "__TEXT")) {
            g_text_start = (uintptr_t)(g_img.segs[i].vmaddr + (uint64_t)g_img.slide);
            g_text_end = g_text_start + (uintptr_t)g_img.segs[i].vmsize;
        }
    }
    mo_section *s;
    if ((s = mo_find_section(&g_img, "__TEXT", "__unwind_info"))) { g_unwind_info = (uintptr_t)s->addr; g_unwind_info_size = (size_t)s->size; }
    if ((s = mo_find_section(&g_img, "__TEXT", "__eh_frame"))) { g_eh_frame = (uintptr_t)s->addr; g_eh_frame_size = (size_t)s->size; }

    if (mo_bind(&g_img, port_resolve_symbol) != 0) { port_report_fatal("Could not link the FTL executable."); return 1; }
    if (patches_apply(&g_img) != 0) LOGW("some patches failed to apply (unexpected game build?)");
    mo_protect(&g_img);
    rt_register_image_objc(&g_img);

    if (cfg->hyperspace) {
        char err[1200];
        if (hyperspace_start(cfg->hyperspace, cfg->log_dir, cfg->docs_dir, &g_img, err, sizeof err) != 0) {
            LOGE("%s", err);
            port_report_fatal(err);
            return 1;
        }
    }

    static const char *argv[] = {"FTL", NULL};
    static const char *envp[] = {"HOME=/", NULL};
    static const char *apple[] = {"executable_path=FTL", NULL};
    LOGI("running static initializers");
    mo_run_initializers(&g_img, 1, argv, envp, apple);
    LOGI("calling main() at %p", (void *)(uintptr_t)g_img.entry);
    int (*main_fn)(int, const char **, const char **, const char **) =
        (int (*)(int, const char **, const char **, const char **))(uintptr_t)g_img.entry;
    return main_fn(1, argv, envp, apple);
}

// ---------------------------------------------------------------------------
// Crash diagnostics: walk the frame-pointer chain (both our code and Apple's
// arm64 code keep x29 frames) and print game addresses at link-time values.
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>

static void describe_addr(uintptr_t pc, char *out, size_t n) {
    if (g_img.base && pc >= (uintptr_t)g_img.base && pc < (uintptr_t)g_img.base + g_img.map_size) {
        snprintf(out, n, "FTL+%#lx", (unsigned long)(pc - (uintptr_t)g_img.slide));
        return;
    }
    Dl_info di;
    if (dladdr((void *)pc, &di) && di.dli_fname) {
        const char *f = strrchr(di.dli_fname, '/');
        snprintf(out, n, "%s!%s+%#lx", f ? f + 1 : di.dli_fname, di.dli_sname ? di.dli_sname : "?",
                 (unsigned long)(pc - (uintptr_t)(di.dli_saddr ? di.dli_saddr : di.dli_fbase)));
        return;
    }
    snprintf(out, n, "%#lx", (unsigned long)pc);
}

void port_backtrace_from(uintptr_t pc, uintptr_t lr, uintptr_t fp, const char *why) {
    char b[256];
    LOGE("==== backtrace (%s) ====", why);
    describe_addr(pc, b, sizeof b);
    LOGE("  pc %s", b);
    describe_addr(lr, b, sizeof b);
    LOGE("  lr %s", b);
    for (int i = 0; i < 48 && fp; i++) {
        uintptr_t *frame = (uintptr_t *)fp;
        uintptr_t ret = frame[1];
        if (!ret) break;
        describe_addr(ret, b, sizeof b);
        LOGE("  #%d %s", i, b);
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
}

static void crash_handler(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
#if defined(__aarch64__)
    uintptr_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30], fp = uc->uc_mcontext.regs[29];
    char why[64];
    snprintf(why, sizeof why, "signal %d addr %p", sig, si->si_addr);
    port_backtrace_from(pc, lr, fp, why);
    for (int i = 0; i < 31; i += 4)
        LOGE("  x%-2d %016llx %016llx %016llx %016llx", i, (unsigned long long)uc->uc_mcontext.regs[i],
             (unsigned long long)uc->uc_mcontext.regs[i + 1], (unsigned long long)(i + 2 < 31 ? uc->uc_mcontext.regs[i + 2] : 0),
             (unsigned long long)(i + 3 < 31 ? uc->uc_mcontext.regs[i + 3] : 0));
#endif
    port_log_mark_crash();
    signal(sig, SIG_DFL);
    raise(sig);
}

void port_install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}
