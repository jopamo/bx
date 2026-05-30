#ifndef BX_LIB_FAULT_INJECTION_H
#define BX_LIB_FAULT_INJECTION_H

#include <stdbool.h>

#include "lib/compiler.h"

#ifndef BX_ENABLE_FAULT_INJECTION
#define BX_ENABLE_FAULT_INJECTION 0
#endif

typedef bool (*bx_fault_injection_decide_fn)(const char* point, void* user);

#if BX_ENABLE_FAULT_INJECTION
bool bx_fault_injection_enabled(void);
void bx_fault_injection_configure(
    bool enabled,
    bx_fault_injection_decide_fn decide,
    void* user
);
bool bx_fault_injection_check_enabled(const char* point);

#define BX_FAULT_INJECTION_POINT(point)                                      \
    (BX_UNLIKELY(bx_fault_injection_enabled()) ?                            \
         bx_fault_injection_check_enabled((point)) :                         \
         false)
#else
static inline bool bx_fault_injection_enabled(void) {
    return false;
}

static inline void bx_fault_injection_configure(
    bool enabled,
    bx_fault_injection_decide_fn decide,
    void* user
) {
    (void)enabled;
    (void)decide;
    (void)user;
}

static inline bool bx_fault_injection_check_enabled(const char* point) {
    (void)point;
    return false;
}

#define BX_FAULT_INJECTION_POINT(point) false
#endif

#endif /* BX_LIB_FAULT_INJECTION_H */
