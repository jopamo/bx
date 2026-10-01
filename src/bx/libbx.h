#ifndef LIBBX_H
#define LIBBX_H

#include <stddef.h>

/* Zero-size requests return storage too; allocation failure exits. */
void* xmalloc(size_t size) __attribute__((returns_nonnull));
void* xrealloc(void* ptr, size_t size);
char* xstrdup(const char* s);

#endif /* LIBBX_H */
