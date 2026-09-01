#ifndef BX_DISPATCH_H
#define BX_DISPATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*bx_applet_main_t)(int argc, char** argv);

/*
 * Zero is deliberately the strongest isolation boundary. Missing,
 * uninitialized, or unknown policy must never grant in-process execution.
 */
enum bx_applet_execution_class {
    BX_APPLET_EXEC_ONLY = 0,
    BX_APPLET_CHILD_IN_PROCESS_SAFE,
    BX_APPLET_PARENT_SHELL_SAFE,
};

enum bx_applet_capability {
    BX_APPLET_CAP_NONE = 0u,
    BX_APPLET_CAP_FILESYSTEM_READ = 1u << 0,
    BX_APPLET_CAP_FILESYSTEM_WRITE = 1u << 1,
    BX_APPLET_CAP_RECURSIVE_TRAVERSAL = 1u << 2,
    BX_APPLET_CAP_CHILD_EXECUTION = 1u << 3,
    BX_APPLET_CAP_NETWORK_ACCESS = 1u << 4,
    BX_APPLET_CAP_TERMINAL_CONTROL = 1u << 5,
    BX_APPLET_CAP_PRIVILEGE_SENSITIVITY = 1u << 6,
    BX_APPLET_CAP_RAW_OUTPUT = 1u << 7,
};

/*
 * The applet registry is immutable runtime state: generated tables are static
 * const data, lookup has no registration/cache path, and callers only receive
 * const entry views.
 */
struct bx_applet {
    const char* name;
    bx_applet_main_t main;
    uint32_t capabilities;
    bool boot_critical;
    enum bx_applet_execution_class execution_class;
};

const struct bx_applet* bx_applet_find(const char* name);
size_t bx_applet_count(void);
const struct bx_applet* bx_applet_at(size_t index);
static inline enum bx_applet_execution_class
bx_applet_execution_class_get(const struct bx_applet* applet) {
    if (applet != NULL) {
        switch (applet->execution_class) {
            case BX_APPLET_EXEC_ONLY:
            case BX_APPLET_CHILD_IN_PROCESS_SAFE:
            case BX_APPLET_PARENT_SHELL_SAFE:
                return applet->execution_class;
        }
    }
    return BX_APPLET_EXEC_ONLY;
}

#endif /* BX_DISPATCH_H */
