#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

#include "copy_data.h"
#include "lib/xreadwrite.h"

static bool is_all_zeros(const char* buf, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (buf[i] != 0)
            return false;
    }
    return true;
}

void bx_copy_buffer_release(struct bx_copy_buffer* buffer) {
    int saved_errno = errno;
    free(buffer->data);
    buffer->data = NULL;
    errno = saved_errno;
}

int bx_copy_data_buffered(int src_fd, int dest_fd, off_t length, enum bx_sparse_mode sparse_mode) {
    struct bx_copy_buffer buffer = {0};
    int result = bx_copy_data_buffered_with_buffer(src_fd, dest_fd, length, sparse_mode, &buffer);
    bx_copy_buffer_release(&buffer);
    return result;
}

int bx_copy_data_buffered_with_buffer(int src_fd, int dest_fd, off_t length,
    enum bx_sparse_mode sparse_mode, struct bx_copy_buffer* workspace) {
    const size_t buffer_size = 65536;
    bool sparse_detected = false;

    if (length < -1) {
        errno = EINVAL;
        return BX_COPY_DATA_READ_ERROR;
    }
    if (length == 0)
        return BX_COPY_DATA_SUCCESS;
    if (workspace->data == NULL)
        workspace->data = malloc(buffer_size);
    char* buffer = workspace->data;
    if (buffer == NULL)
        return BX_COPY_DATA_READ_ERROR;

    while (length != 0) {
        size_t chunk = buffer_size;
        if (length > 0 && length < (off_t)chunk)
            chunk = (size_t)length;
        ssize_t nread = bx_xread(src_fd, buffer, chunk);
        if (nread < 0)
            return BX_COPY_DATA_READ_ERROR;
        if (nread == 0) {
            if (length > 0) {
                errno = EIO;
                return BX_COPY_DATA_READ_ERROR;
            }
            break;
        }

        bool all_zeros = sparse_mode != BX_SPARSE_NEVER
            && is_all_zeros(buffer, (size_t)nread);
        if (all_zeros && (sparse_mode == BX_SPARSE_ALWAYS || (size_t)nread == buffer_size)) {
            if (lseek(dest_fd, nread, SEEK_CUR) < 0)
                return BX_COPY_DATA_WRITE_ERROR;
            sparse_detected = true;
        }
        else if (!bx_xwrite_all(dest_fd, buffer, (size_t)nread)) {
            return BX_COPY_DATA_WRITE_ERROR;
        }
        if (length > 0)
            length -= nread;
    }

    if (sparse_detected) {
        off_t end = lseek(dest_fd, 0, SEEK_CUR);
        if (end < 0 || ftruncate(dest_fd, end) != 0)
            return BX_COPY_DATA_WRITE_ERROR;
    }
    return BX_COPY_DATA_SUCCESS;
}
