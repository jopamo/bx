#ifndef BX_COMMON_COPY_PLATFORM_H
#define BX_COMMON_COPY_PLATFORM_H

#include <sys/types.h>

/* Borrow both descriptors. Clone the whole regular file into an empty
 * destination, returning zero on success or -1 with errno on failure.
 * An unavailable implementation returns ENOTSUP without changing either fd. */
int bx_copy_clone_file(int src_fd, int dest_fd);

/* Copy at the current offsets, advancing both by the returned byte count.
 * Short copies and zero returns are left to the caller. Return -1 with errno
 * on failure; an unavailable implementation leaves both offsets unchanged. */
ssize_t bx_copy_file_range(int src_fd, int dest_fd, size_t length);

#endif
