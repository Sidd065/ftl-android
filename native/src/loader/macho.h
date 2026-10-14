// Minimal arm64 Mach-O loader (DYLD_INFO style fixups, no chained fixups).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct mo_section {
    char segname[17];
    char sectname[17];
    uint64_t addr;  // runtime (slid) address
    uint64_t size;
    uint32_t flags;
} mo_section;

typedef struct mo_segment {
    char name[17];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    int initprot;
} mo_segment;

typedef struct mo_image {
    char path[512];
    uint8_t *file;       // arm64 slice, malloc'd copy
    size_t file_size;
    uint8_t *base;       // runtime address of the lowest mapped segment (holds mach_header)
    uint64_t vmaddr_min; // link-time address of base
    intptr_t slide;      // runtime = link-time + slide
    size_t map_size;
    mo_segment segs[16];
    int nsegs;
    mo_section sects[64];
    int nsects;
    char dylibs[32][160];
    int ndylibs;
    uint64_t entry;      // runtime address of main()
    uint32_t rebase_off, rebase_size, bind_off, bind_size, weak_off, weak_size;
    uint32_t lazy_off, lazy_size, export_off, export_size;
    uint32_t symoff, nsyms, stroff, strsize;
    uint32_t fstarts_off, fstarts_size;
    uint8_t *near_tramp;  // RX after mo_protect; trampolines within +-128MB of the code
    size_t near_tramp_size, near_tramp_used;
} mo_image;

// Resolve an imported symbol. `dylib` is the install name it is imported from (may be NULL).
// Return NULL if unknown; the loader then binds a trap stub (functions) or zero page (data).
typedef void *(*mo_resolver)(const char *sym, const char *dylib);

int mo_load(const char *path, mo_image *img);
int mo_bind(mo_image *img, mo_resolver resolve);
void mo_protect(mo_image *img);
void mo_run_initializers(mo_image *img, int argc, const char **argv, const char **envp, const char **apple);
mo_section *mo_find_section(mo_image *img, const char *seg, const char *sect);
static inline void *mo_ptr(mo_image *img, uint64_t linkaddr) { return (void *)(uintptr_t)(linkaddr + img->slide); }

// Unresolved-import diagnostics
void mo_set_trap_handler(void (*fn)(const char *name));
