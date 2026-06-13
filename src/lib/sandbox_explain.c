#include "lib/sandbox_explain.h"

#include <stdatomic.h>

static atomic_bool bx_sandbox_explain_runtime_enabled;

bool bx_sandbox_explain_enabled(void) {
    return atomic_load_explicit(&bx_sandbox_explain_runtime_enabled, memory_order_relaxed);
}

void bx_sandbox_explain_set_enabled(bool enabled) {
    atomic_store_explicit(&bx_sandbox_explain_runtime_enabled, enabled, memory_order_relaxed);
}

void bx_sandbox_explain_emit_enabled(bx_sandbox_explain_emit_fn emit, void* user) {
    if (emit) {
        emit(user);
    }
}
