#ifndef LIBBX_H
#define LIBBX_H

#include <stddef.h>

void* xmalloc(size_t size);
void* xrealloc(void* ptr, size_t size);
char* xstrdup(const char* s);

#endif /* LIBBX_H */
