#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <errno.h>

#include "copy_data.h"
#include "lib/fd_ops.h"

static bool bx_copy_sparse_auto_seek_unsupported(int errnum) {
    if (errnum == EINVAL || errnum == ENOTSUP || errnum == ENOSYS) {
        return true;
    }
#ifdef EOPNOTSUPP
    if (errnum == EOPNOTSUPP) {
        return true;
    }
#endif
    return false;
}

static int bx_copy_data_sparse_auto(int src_fd, int dest_fd, bool* handled_out) {
    struct stat src_stat;
    off_t offset = 0;
    bool preserved_hole = false;

    *handled_out = false;

    if (fstat(src_fd, &src_stat) != 0) {
        return BX_COPY_DATA_READ_ERROR;
    }
    if (!S_ISREG(src_stat.st_mode)) {
        return BX_COPY_DATA_SUCCESS;
    }
    if (src_stat.st_size == 0) {
        /* procfs files can report zero size while still supplying data. */
        return BX_COPY_DATA_SUCCESS;
    }

    while (offset < src_stat.st_size) {
        off_t data_offset, hole_offset;
        int extent = bx_fd_next_data_extent(src_fd, offset, src_stat.st_size, &data_offset, &hole_offset);
        if (extent == 0) {
            preserved_hole = true;
            break;
        }
        if (extent < 0) {
            if (offset == 0 && bx_copy_sparse_auto_seek_unsupported(errno)) {
                if (lseek(src_fd, 0, SEEK_SET) < 0) {
                    return BX_COPY_DATA_READ_ERROR;
                }
                if (lseek(dest_fd, 0, SEEK_SET) < 0) {
                    return BX_COPY_DATA_WRITE_ERROR;
                }
                return BX_COPY_DATA_SUCCESS;
            }
            *handled_out = true;
            return BX_COPY_DATA_READ_ERROR;
        }

        if (data_offset > offset) {
            if (lseek(dest_fd, data_offset, SEEK_SET) < 0) {
                *handled_out = true;
                return BX_COPY_DATA_WRITE_ERROR;
            }
            preserved_hole = true;
        }

        if (lseek(src_fd, data_offset, SEEK_SET) < 0) {
            *handled_out = true;
            return BX_COPY_DATA_READ_ERROR;
        }
        if (lseek(dest_fd, data_offset, SEEK_SET) < 0) {
            *handled_out = true;
            return BX_COPY_DATA_WRITE_ERROR;
        }

        int copy_res = bx_copy_data_buffered(src_fd, dest_fd, hole_offset - data_offset, BX_SPARSE_NEVER);
        if (copy_res != BX_COPY_DATA_SUCCESS) {
            *handled_out = true;
            return copy_res;
        }

        offset = hole_offset;
    }

    if (preserved_hole && ftruncate(dest_fd, src_stat.st_size) != 0) {
        *handled_out = true;
        return BX_COPY_DATA_WRITE_ERROR;
    }

    *handled_out = true;
    return BX_COPY_DATA_SUCCESS;
}

int bx_copy_data(int src_fd, int dest_fd, const struct bx_copy_data_options* opts) {
    enum bx_sparse_mode sparse_mode = opts->sparse_mode;

    if (opts->reflink_mode != BX_REFLINK_NEVER) {
        if (ioctl(dest_fd, FICLONE, src_fd) == 0) {
            return BX_COPY_DATA_SUCCESS;
        }
        if (opts->reflink_mode == BX_REFLINK_ALWAYS) {
            return BX_COPY_DATA_REFLINK_FAILED;
        }
    }

    if (sparse_mode != BX_SPARSE_NEVER) {
        struct stat dest_stat;
        if (fstat(dest_fd, &dest_stat) != 0) {
            return BX_COPY_DATA_WRITE_ERROR;
        }
        if (!S_ISREG(dest_stat.st_mode)) {
            sparse_mode = BX_SPARSE_NEVER;
        }
    }

    if (sparse_mode == BX_SPARSE_AUTO) {
        bool handled = false;
        int res = bx_copy_data_sparse_auto(src_fd, dest_fd, &handled);
        if (handled || res != BX_COPY_DATA_SUCCESS) {
            return res;
        }
        return bx_copy_data_buffered(src_fd, dest_fd, -1, BX_SPARSE_NEVER);
    }

    return bx_copy_data_buffered(src_fd, dest_fd, -1, sparse_mode);
}
