#ifndef BX_LIB_SANDBOX_EXPLAIN_H
#define BX_LIB_SANDBOX_EXPLAIN_H

#include <stdbool.h>

#include "lib/compiler.h"

typedef void (*bx_sandbox_explain_emit_fn)(void* user);

bool bx_sandbox_explain_enabled(void);
void bx_sandbox_explain_set_enabled(bool enabled);
void bx_sandbox_explain_emit_enabled(bx_sandbox_explain_emit_fn emit, void* user);

#define BX_SANDBOX_EXPLAIN(emit, user)                                        \
    do {                                                                     \
        if (BX_UNLIKELY(bx_sandbox_explain_enabled())) {                     \
            bx_sandbox_explain_emit_enabled((emit), (user));                 \
        }                                                                    \
    } while (0)

#endif /* BX_LIB_SANDBOX_EXPLAIN_H */
