#include <errno.h>

#include "copy_platform.h"

int bx_copy_clone_file(int src_fd, int dest_fd) {
    (void)src_fd;
    (void)dest_fd;
    errno = ENOTSUP;
    return -1;
}
