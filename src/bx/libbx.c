#include <stdlib.h>
#include <string.h>

#include "bx/libbx.h"
#include "bx/diag.h"
#include "lib/compiler.h"

static BX_COLD void bx_oom_fatal(const char* message) {
    bx_pfatal(3, message);
}

void* xmalloc(size_t size) {
    void* p = malloc(size);
    if (BX_UNLIKELY(p == NULL && size > 0u)) {
        bx_oom_fatal("malloc failure");
    }
    return p;
}

void* xrealloc(void* ptr, size_t size) {
    void* p = realloc(ptr, size);
    if (BX_UNLIKELY(p == NULL && size > 0u)) {
        bx_oom_fatal("realloc failure");
    }
    return p;
}

char* xstrdup(const char* s) {
    char* p = strdup(s);
    if (BX_UNLIKELY(p == NULL)) {
        bx_oom_fatal("strdup failure");
    }
    return p;
}
