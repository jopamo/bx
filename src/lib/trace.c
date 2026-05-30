#include "lib/trace.h"

#if BX_ENABLE_TRACEPOINTS
#include <stdatomic.h>

static atomic_bool bx_trace_runtime_enabled;

bool bx_trace_enabled(void) {
    return atomic_load_explicit(&bx_trace_runtime_enabled, memory_order_relaxed);
}

void bx_trace_set_enabled(bool enabled) {
    atomic_store_explicit(&bx_trace_runtime_enabled, enabled, memory_order_relaxed);
}

void bx_trace_emit_enabled(const char* event, bx_trace_emit_fn emit, void* user) {
    if (emit != NULL) {
        emit(event, user);
    }
}
#else
typedef int bx_trace_disabled_translation_unit;
#endif
