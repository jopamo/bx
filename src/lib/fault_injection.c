#include "lib/fault_injection.h"

#if BX_ENABLE_FAULT_INJECTION
#include <stdatomic.h>

static atomic_bool bx_fault_injection_runtime_enabled;
static bx_fault_injection_decide_fn bx_fault_injection_decide;
static void* bx_fault_injection_user;

bool bx_fault_injection_enabled(void) {
    return atomic_load_explicit(&bx_fault_injection_runtime_enabled, memory_order_relaxed);
}

void bx_fault_injection_configure(
    bool enabled,
    bx_fault_injection_decide_fn decide,
    void* user
) {
    bx_fault_injection_decide = decide;
    bx_fault_injection_user = user;
    atomic_store_explicit(&bx_fault_injection_runtime_enabled, enabled, memory_order_relaxed);
}

bool bx_fault_injection_check_enabled(const char* point) {
    bx_fault_injection_decide_fn decide = bx_fault_injection_decide;

    if (decide == NULL) {
        return false;
    }
    return decide(point, bx_fault_injection_user);
}
#else
typedef int bx_fault_injection_disabled_translation_unit;
#endif
