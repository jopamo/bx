#ifndef BX_LIB_TRACE_H
#define BX_LIB_TRACE_H

#include <stdbool.h>

#include "lib/compiler.h"

#ifndef BX_ENABLE_TRACEPOINTS
#define BX_ENABLE_TRACEPOINTS 0
#endif

typedef void (*bx_trace_emit_fn)(const char* event, void* user);

#if BX_ENABLE_TRACEPOINTS
bool bx_trace_enabled(void);
void bx_trace_set_enabled(bool enabled);
void bx_trace_emit_enabled(const char* event, bx_trace_emit_fn emit, void* user);

#define BX_TRACEPOINT(event, emit, user)                                      \
    do {                                                                     \
        if (BX_UNLIKELY(bx_trace_enabled())) {                               \
            bx_trace_emit_enabled((event), (emit), (user));                  \
        }                                                                    \
    } while (0)
#else
static inline bool bx_trace_enabled(void) {
    return false;
}

static inline void bx_trace_set_enabled(bool enabled) {
    (void)enabled;
}

#define BX_TRACEPOINT(event, emit, user) \
    do {                                \
    } while (0)
#endif

#endif /* BX_LIB_TRACE_H */
