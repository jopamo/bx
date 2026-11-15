#define _GNU_SOURCE
#undef _POSIX_C_SOURCE

#include "fd_ops.h"

#ifndef BX_HAVE_SEEK_DATA_HOLE
#define BX_HAVE_SEEK_DATA_HOLE 0
#endif

int bx_fd_next_data_extent(int fd, off_t offset, off_t size, off_t* begin, off_t* end) {
    if (!begin || !end || offset < 0 || size < 0 || offset > size) {
        errno = EINVAL;
        return -1;
    }
#if BX_HAVE_SEEK_DATA_HOLE
    off_t data = lseek(fd, offset, SEEK_DATA);
    if (data < 0)
        return errno == ENXIO ? 0 : -1;
    if (data < offset || data >= size) {
        errno = ESTALE;
        return -1;
    }
    off_t hole = lseek(fd, data, SEEK_HOLE);
    if (hole < 0)
        return -1;
    if (hole <= data || hole > size) {
        errno = ESTALE;
        return -1;
    }
    *begin = data;
    *end = hole;
    return 1;
#else
    (void)fd;
    errno = ENOTSUP;
    return -1;
#endif
}
