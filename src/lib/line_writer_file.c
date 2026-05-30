#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lib/line_writer_file.h"

static int bx_line_writer_file_set_error(struct bx_line_writer_file *file) {
    int saved_errno = errno != 0 ? errno : EIO;

    if (file)
        file->error = saved_errno;
    errno = saved_errno;
    return -1;
}

static ssize_t bx_line_writer_file_cookie_write(void *cookie,
                                                const char *data,
                                                size_t len) {
    struct bx_line_writer_file *file = cookie;

    if (!file || (!data && len > 0u)) {
        errno = EINVAL;
        return -1;
    }
    if (file->error != 0) {
        errno = file->error;
        return -1;
    }
    if (!bx_line_writer_write(&file->writer, data, len))
        return bx_line_writer_file_set_error(file);
    if (len > (size_t)SSIZE_MAX)
        return SSIZE_MAX;
    return (ssize_t)len;
}

static int bx_line_writer_file_cookie_close(void *cookie) {
    struct bx_line_writer_file *file = cookie;
    int saved_errno = 0;

    if (!file) {
        errno = EINVAL;
        return -1;
    }
    if (file->error != 0)
        saved_errno = file->error;
    else if (!bx_line_writer_flush(&file->writer))
        saved_errno = errno != 0 ? errno : EIO;
    if (file->close_fd && file->fd >= 0) {
        int fd = file->fd;

        file->fd = -1;
        file->close_fd = false;
        if (close(fd) != 0 && saved_errno == 0)
            saved_errno = errno != 0 ? errno : EIO;
    }
    if (saved_errno != 0) {
        file->error = saved_errno;
        errno = saved_errno;
        return -1;
    }
    return 0;
}

bool bx_line_writer_file_open(struct bx_line_writer_file *file, int fd, bool close_fd) {
    cookie_io_functions_t io = {
        .read = NULL,
        .write = bx_line_writer_file_cookie_write,
        .seek = NULL,
        .close = bx_line_writer_file_cookie_close,
    };
    FILE *stream;

    if (!file || fd < 0) {
        errno = EINVAL;
        return false;
    }

    memset(file, 0, sizeof(*file));
    file->fd = fd;
    file->close_fd = close_fd;
    bx_line_writer_init(&file->writer, fd, file->buffer, sizeof(file->buffer));

    stream = fopencookie(file, "w", io);
    if (!stream) {
        int saved_errno = errno != 0 ? errno : EIO;

        if (close_fd)
            close(fd);
        file->fd = -1;
        file->close_fd = false;
        file->error = saved_errno;
        errno = saved_errno;
        return false;
    }
    (void)setvbuf(stream, NULL, _IONBF, 0);
    file->stream = stream;
    return true;
}

FILE *bx_line_writer_file_stream(struct bx_line_writer_file *file) {
    return file ? file->stream : NULL;
}

bool bx_line_writer_file_flush(struct bx_line_writer_file *file) {
    if (!file) {
        errno = EINVAL;
        return false;
    }
    if (file->error != 0) {
        errno = file->error;
        return false;
    }
    if (!bx_line_writer_flush(&file->writer)) {
        file->error = errno != 0 ? errno : EIO;
        errno = file->error;
        return false;
    }
    return true;
}

bool bx_line_writer_file_finish(struct bx_line_writer_file *file) {
    FILE *stream;

    if (!file) {
        errno = EINVAL;
        return false;
    }

    stream = file->stream;
    if (!stream) {
        if (file->error != 0) {
            errno = file->error;
            return false;
        }
        return true;
    }

    file->stream = NULL;
    if (fclose(stream) != 0) {
        int saved_errno = errno != 0 ? errno : EIO;

        file->error = saved_errno;
        errno = saved_errno;
        return false;
    }
    return true;
}

void bx_line_writer_file_cleanup(struct bx_line_writer_file *file) {
    if (!file)
        return;

    if (file->stream) {
        FILE *stream = file->stream;

        file->stream = NULL;
        (void)fclose(stream);
    }
    if (file->close_fd && file->fd >= 0) {
        (void)close(file->fd);
        file->fd = -1;
        file->close_fd = false;
    }
}

int bx_line_writer_file_error(const struct bx_line_writer_file *file) {
    if (!file)
        return EINVAL;
    if (file->error != 0)
        return file->error;
    return bx_line_writer_error(&file->writer);
}
