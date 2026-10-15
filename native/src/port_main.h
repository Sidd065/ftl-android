#pragma once
#include <stdint.h>

typedef struct {
    const char *binary_path;  // the iOS Mach-O executable (FTL)
    const char *bundle_dir;   // directory holding ftl.dat (the ".app" resources)
    const char *docs_dir;     // writable: saves, settings
    const char *cache_dir;    // writable: caches
    const char *hw_machine;   // reported sysctl hw.machine
    int verbose;
    const char *mods_index;   // overlay of modded files (see ux/mods.c), or NULL
    const char *hyperspace;   // libHyperspace.so to start before the game (see hyperspace.c), or NULL
    const char *log_dir;      // where the session logs go (Hyperspace's logs go there too)
} port_config;

int port_run_game(const port_config *cfg);   // never returns on success (UIApplicationMain)
void port_report_fatal(const char *msg);     // platform: show the user a fatal error
void port_on_game_exit(int code);            // platform: game called exit()

struct mo_image;
struct mo_image *port_image(void);
uintptr_t port_image_slide(void);
const char *port_mods_index(void);
void port_install_crash_handler(void);
void port_backtrace_from(uintptr_t pc, uintptr_t lr, uintptr_t fp, const char *why);
