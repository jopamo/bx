#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <errno.h>
#include <string.h>

#include "copy_data.h"

static bool is_all_zeros(const char *buf, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (buf[i] != 0) return false;
    }
    return true;
}

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

static int bx_copy_data_buffered(int src_fd,
                                 int dest_fd,
                                 enum bx_sparse_mode sparse_mode) {
    char buffer[65536];
    off_t last_write_end = 0;
    bool sparse_detected = false;

    while (true) {
        ssize_t nread = read(src_fd, buffer, sizeof(buffer));
        if (nread == 0) {
            if (sparse_detected) {
                if (ftruncate(dest_fd, last_write_end) != 0) {
                    return BX_COPY_DATA_WRITE_ERROR;
                }
            }
            return BX_COPY_DATA_SUCCESS;
        }
        if (nread < 0) {
            return BX_COPY_DATA_READ_ERROR;
        }

        bool all_zeros = false;
        if (sparse_mode != BX_SPARSE_NEVER) {
            all_zeros = is_all_zeros(buffer, (size_t)nread);
        }

        if (all_zeros && (sparse_mode == BX_SPARSE_ALWAYS || nread == sizeof(buffer))) {
            if (lseek(dest_fd, nread, SEEK_CUR) < 0) {
                return BX_COPY_DATA_WRITE_ERROR;
            }
            last_write_end += nread;
            sparse_detected = true;
        } else {
            ssize_t written_total = 0;
            while (written_total < nread) {
                ssize_t nwritten = write(dest_fd,
                                         buffer + written_total,
                                         (size_t)(nread - written_total));
                if (nwritten < 0) {
                    return BX_COPY_DATA_WRITE_ERROR;
                }
                written_total += nwritten;
            }
            last_write_end += nread;
        }
    }
}

static int bx_copy_data_copy_range(int src_fd,
                                   int dest_fd,
                                   off_t length) {
    char buffer[65536];
    off_t remaining = length;

    while (remaining > 0) {
        size_t chunk = sizeof(buffer);
        if ((off_t)chunk > remaining) {
            chunk = (size_t)remaining;
        }

        ssize_t nread = read(src_fd, buffer, chunk);
        if (nread <= 0) {
            return BX_COPY_DATA_READ_ERROR;
        }

        ssize_t written_total = 0;
        while (written_total < nread) {
            ssize_t nwritten = write(dest_fd,
                                     buffer + written_total,
                                     (size_t)(nread - written_total));
            if (nwritten < 0) {
                return BX_COPY_DATA_WRITE_ERROR;
            }
            written_total += nwritten;
        }

        remaining -= nread;
    }

    return BX_COPY_DATA_SUCCESS;
}

static int bx_copy_data_sparse_auto(int src_fd,
                                    int dest_fd,
                                    bool *handled_out) {
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
        *handled_out = true;
        return BX_COPY_DATA_SUCCESS;
    }

    while (offset < src_stat.st_size) {
        off_t data_offset = lseek(src_fd, offset, SEEK_DATA);
        if (data_offset < 0) {
            if (errno == ENXIO) {
                preserved_hole = true;
                break;
            }
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

        off_t hole_offset = lseek(src_fd, data_offset, SEEK_HOLE);
        if (hole_offset < 0) {
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
        if (hole_offset <= data_offset) {
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

        int copy_res = bx_copy_data_copy_range(src_fd, dest_fd, hole_offset - data_offset);
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

int bx_copy_data(int src_fd, int dest_fd, const struct bx_copy_data_options *opts) {
    if (opts->reflink_mode != BX_REFLINK_NEVER) {
        if (ioctl(dest_fd, FICLONE, src_fd) == 0) {
            return BX_COPY_DATA_SUCCESS;
        }
        if (opts->reflink_mode == BX_REFLINK_ALWAYS) {
            return BX_COPY_DATA_REFLINK_FAILED;
        }
    }

    if (opts->sparse_mode == BX_SPARSE_AUTO) {
        bool handled = false;
        int res = bx_copy_data_sparse_auto(src_fd, dest_fd, &handled);
        if (handled) {
            return res;
        }
        return bx_copy_data_buffered(src_fd, dest_fd, BX_SPARSE_NEVER);
    }

    return bx_copy_data_buffered(src_fd, dest_fd, opts->sparse_mode);
}
