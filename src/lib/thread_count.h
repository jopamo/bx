#ifndef BX_LIB_THREAD_COUNT_H
#define BX_LIB_THREAD_COUNT_H

#include <stdbool.h>
#include <stddef.h>

bool bx_thread_count_parse(const char *progname,
                           const char *optname,
                           const char *text,
                           int *out);
size_t bx_thread_count_resolve(int requested);

#endif
