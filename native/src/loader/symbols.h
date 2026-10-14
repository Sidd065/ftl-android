#pragma once
typedef struct {
    const char *name;
    void *addr;
} sym_entry;

extern const sym_entry g_cxx_syms[];
void symbols_init(void);
void *port_resolve_symbol(const char *sym, const char *dylib);
