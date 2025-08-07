#include <stdlib.h>
#include <string.h>
#include "bx/libbx.h"
#include "bx/diag.h"

void* xmalloc(size_t size) {
    void* p = malloc(size);
    if (!p && size > 0) {
        bx_pfatal(3, "malloc failure");
    }
    return p;
}

void* xrealloc(void* ptr, size_t size) {
    void* p = realloc(ptr, size);
    if (!p && size > 0) {
        bx_pfatal(3, "realloc failure");
    }
    return p;
}

char* xstrdup(const char* s) {
    char* p = strdup(s);
    if (!p) {
        bx_pfatal(3, "strdup failure");
    }
    return p;
}
