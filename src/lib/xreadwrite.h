#ifndef BX_COMMON_XREADWRITE_H
#define BX_COMMON_XREADWRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

enum bx_xwrite_status {
    BX_XWRITE_OK = 0,
    BX_XWRITE_ERROR,
    BX_XWRITE_ZERO,
};

ssize_t bx_xread(int fd, void* buffer, size_t count);
enum bx_xwrite_status bx_xwrite_all_status(int fd, const void* buffer, size_t count);
bool bx_xwrite_all(int fd, const void* buffer, size_t count);

#endif /* BX_COMMON_XREADWRITE_H */
