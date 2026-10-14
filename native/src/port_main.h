#pragma once
#include <stdint.h>

typedef struct {
    const char *binary_path;  // the iOS Mach-O executable (FTL)
    const char *bundle_dir;   // directory holding ftl.dat (the ".app" resources)
    const char *docs_dir;     // writable: saves, settings
    const char *cache_dir;    // writable: caches
    const char *hw_machine;   // reported sysctl hw.machine
    int verbose;
} port_config;

int port_run_game(const port_config *cfg);   // never returns on success (UIApplicationMain)
void port_report_fatal(const char *msg);     // platform: show the user a fatal error
void port_on_game_exit(int code);            // platform: game called exit()

struct mo_image;
struct mo_image *port_image(void);
uintptr_t port_image_slide(void);
void port_install_crash_handler(void);
void port_backtrace_from(uintptr_t pc, uintptr_t lr, uintptr_t fp, const char *why);
