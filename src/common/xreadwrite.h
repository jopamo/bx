#ifndef BX_COMMON_XREADWRITE_H
#define BX_COMMON_XREADWRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

ssize_t bx_xread(int fd, void* buffer, size_t count);
bool bx_xwrite_all(int fd, const void* buffer, size_t count);

#endif /* BX_COMMON_XREADWRITE_H */
