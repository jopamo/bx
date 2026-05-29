#ifndef BX_DISPATCH_H
#define BX_DISPATCH_H

#include <stdbool.h>
#include <stddef.h>

typedef int (*bx_applet_main_t)(int argc, char** argv);

/*
 * Dispatch is immutable runtime state: generated tables are static const data,
 * lookup has no registration/cache path, and callers only receive const entry
 * views.
 */
struct bx_dispatch_entry {
    const char* name;
    bx_applet_main_t main;
    bool boot_critical;
};

bx_applet_main_t bx_dispatch_find(const char* name);
size_t bx_dispatch_count(void);
const struct bx_dispatch_entry* bx_dispatch_at(size_t index);

#endif /* BX_DISPATCH_H */
