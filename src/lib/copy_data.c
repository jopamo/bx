#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "copy_data.h"
#include "copy_platform.h"
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

static int bx_copy_data_copy_range(int src_fd, int dest_fd, off_t length, bool* use_copy_range,
    struct bx_copy_buffer* buffer) {
    while (*use_copy_range && length > 0) {
        size_t chunk = length > 1024 * 1024 * 1024 ? 1024 * 1024 * 1024 : (size_t)length;
        ssize_t copied = bx_copy_file_range(src_fd, dest_fd, chunk);
        if (copied > 0) {
            length -= copied;
            continue;
        }
        if (copied < 0) {
            if (errno == EINTR)
                continue;
            if (errno != ENOSYS && errno != EXDEV && errno != EINVAL &&
                errno != EOPNOTSUPP && errno != ENOTSUP)
                return BX_COPY_DATA_RANGE_ERROR;
        }
        /* Keep offsets after partial progress; do not retry unsupported
         * offloading for each remaining extent. Verify zero returns by read. */
        *use_copy_range = false;
    }
    return bx_copy_data_buffered_with_buffer(src_fd, dest_fd, length, BX_SPARSE_NEVER, buffer);
}

static int bx_copy_data_sparse_auto(int src_fd, int dest_fd, const struct stat* src_stat,
    bool use_copy_range, bool* handled_out, struct bx_copy_buffer* buffer) {
    off_t offset = 0;
    bool preserved_hole = false;

    *handled_out = false;

    if (!S_ISREG(src_stat->st_mode)) {
        return BX_COPY_DATA_SUCCESS;
    }
    if (src_stat->st_size == 0) {
        /* procfs files can report zero size while still supplying data. */
        return BX_COPY_DATA_SUCCESS;
    }

    while (offset < src_stat->st_size) {
        off_t data_offset, hole_offset;
        int extent;
        do {
            extent = bx_fd_next_data_extent(src_fd, offset, src_stat->st_size, &data_offset, &hole_offset);
        } while (extent < 0 && errno == EINTR);
        if (extent == 0) {
            preserved_hole = true;
            break;
        }
        if (extent < 0) {
            *handled_out = true;
            if (bx_copy_sparse_auto_seek_unsupported(errno)) {
                if (lseek(src_fd, offset, SEEK_SET) < 0) {
                    return BX_COPY_DATA_READ_ERROR;
                }
                if (lseek(dest_fd, offset, SEEK_SET) < 0) {
                    return BX_COPY_DATA_WRITE_ERROR;
                }
                /* Extent probes can move the input offset even on failure.
                 * Scan zeros only when allocation metadata indicates holes. */
                bool sparse = src_stat->st_blocks < src_stat->st_size / 512 +
                    (src_stat->st_size % 512 != 0);
                return bx_copy_data_buffered_with_buffer(src_fd, dest_fd, -1,
                    sparse ? BX_SPARSE_ALWAYS : BX_SPARSE_NEVER, buffer);
            }
            return BX_COPY_DATA_READ_ERROR;
        }

        if (data_offset > offset) {
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

        int copy_res = bx_copy_data_copy_range(src_fd, dest_fd, hole_offset - data_offset, &use_copy_range, buffer);
        if (copy_res != BX_COPY_DATA_SUCCESS) {
            *handled_out = true;
            return copy_res;
        }

        offset = hole_offset;
    }

    if (preserved_hole && ftruncate(dest_fd, src_stat->st_size) != 0) {
        *handled_out = true;
        return BX_COPY_DATA_WRITE_ERROR;
    }

    *handled_out = true;
    return BX_COPY_DATA_SUCCESS;
}

int bx_copy_data(int src_fd, int dest_fd, const struct bx_copy_data_options* opts) {
    struct bx_copy_buffer buffer = {0};
    int result = bx_copy_data_with_buffer(src_fd, dest_fd, opts, &buffer);
    bx_copy_buffer_release(&buffer);
    return result;
}

int bx_copy_data_with_buffer(int src_fd, int dest_fd, const struct bx_copy_data_options* opts,
    struct bx_copy_buffer* buffer) {
    enum bx_sparse_mode sparse_mode = opts->sparse_mode;
    struct stat src_stat, dest_stat;

    if (fstat(src_fd, &src_stat) != 0)
        return BX_COPY_DATA_READ_ERROR;
    if (fstat(dest_fd, &dest_stat) != 0)
        return BX_COPY_DATA_WRITE_ERROR;
    if (S_ISREG(src_stat.st_mode) && S_ISREG(dest_stat.st_mode) &&
        src_stat.st_dev == dest_stat.st_dev && src_stat.st_ino == dest_stat.st_ino) {
        errno = EINVAL;
        return BX_COPY_DATA_WRITE_ERROR;
    }

    /* Whole-file cloning preserves the source allocation, not an explicit
     * request to create holes or materialize them. */
    if (opts->reflink_mode == BX_REFLINK_ALWAYS ||
        (opts->reflink_mode == BX_REFLINK_AUTO && sparse_mode == BX_SPARSE_AUTO)) {
        int cloned = -1;
        errno = EOPNOTSUPP;
        if (sparse_mode == BX_SPARSE_AUTO &&
            S_ISREG(src_stat.st_mode) && S_ISREG(dest_stat.st_mode)) {
            do {
                cloned = bx_copy_clone_file(src_fd, dest_fd);
            } while (cloned < 0 && errno == EINTR);
        }
        if (cloned == 0)
            return BX_COPY_DATA_SUCCESS;
        if (opts->reflink_mode == BX_REFLINK_ALWAYS ||
            (errno != ENOSYS && errno != EOPNOTSUPP && errno != ENOTSUP &&
             errno != ENOTTY && errno != EINVAL && errno != EXDEV)) {
            return BX_COPY_DATA_REFLINK_FAILED;
        }
    }

    if (!S_ISREG(dest_stat.st_mode)) {
        sparse_mode = BX_SPARSE_NEVER;
    }

    if (sparse_mode == BX_SPARSE_AUTO) {
        bool handled = false;
        /* copy_file_range may clone extents, so --reflink=never bypasses it. */
        int res = bx_copy_data_sparse_auto(src_fd, dest_fd, &src_stat,
            opts->reflink_mode != BX_REFLINK_NEVER, &handled, buffer);
        if (handled || res != BX_COPY_DATA_SUCCESS) {
            return res;
        }
        return bx_copy_data_buffered_with_buffer(src_fd, dest_fd, -1, BX_SPARSE_NEVER, buffer);
    }

    return bx_copy_data_buffered_with_buffer(src_fd, dest_fd, -1, sparse_mode, buffer);
}
