#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/ioctl.h>
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

int bx_copy_data(int src_fd, int dest_fd, const struct bx_copy_data_options *opts) {
    if (opts->reflink_mode != BX_REFLINK_NEVER) {
        if (ioctl(dest_fd, FICLONE, src_fd) == 0) {
            return BX_COPY_DATA_SUCCESS;
        }
        if (opts->reflink_mode == BX_REFLINK_ALWAYS) {
            return BX_COPY_DATA_REFLINK_FAILED;
        }
    }

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
        if (opts->sparse_mode != BX_SPARSE_NEVER) {
            all_zeros = is_all_zeros(buffer, (size_t)nread);
        }

        if (all_zeros && (opts->sparse_mode == BX_SPARSE_ALWAYS || nread == sizeof(buffer))) {
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
