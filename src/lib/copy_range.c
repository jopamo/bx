#define _GNU_SOURCE
#undef _POSIX_C_SOURCE

#include <errno.h>
#include <unistd.h>

#include "copy_platform.h"

#ifndef BX_HAVE_COPY_FILE_RANGE
#define BX_HAVE_COPY_FILE_RANGE 0
#endif

ssize_t bx_copy_file_range(int src_fd, int dest_fd, size_t length) {
#if BX_HAVE_COPY_FILE_RANGE
    return copy_file_range(src_fd, NULL, dest_fd, NULL, length, 0);
#else
    (void)src_fd;
    (void)dest_fd;
    (void)length;
    errno = ENOTSUP;
    return -1;
#endif
}
