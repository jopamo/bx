#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lib/line_writer.h"
#include "lib/xreadwrite.h"

static bool bx_line_writer_failed(struct bx_line_writer *writer) {
    if (writer && writer->error != 0) {
        errno = writer->error;
    }
    return false;
}

static bool bx_line_writer_set_error(struct bx_line_writer *writer) {
    int saved_errno = errno != 0 ? errno : EIO;

    if (writer) {
        writer->error = saved_errno;
    }
    errno = saved_errno;
    return false;
}

void bx_line_writer_init(struct bx_line_writer *writer, int fd, char *buffer, size_t capacity) {
    if (!writer) {
        return;
    }

    writer->fd = fd;
    writer->buffer = buffer;
    writer->capacity = buffer ? capacity : 0u;
    writer->length = 0u;
    writer->error = 0;
}

bool bx_line_writer_flush(struct bx_line_writer *writer) {
    if (!writer) {
        errno = EINVAL;
        return false;
    }
    if (writer->error != 0) {
        return bx_line_writer_failed(writer);
    }
    if (writer->length == 0u) {
        return true;
    }

    size_t length = writer->length;
    writer->length = 0u;
    if (!bx_xwrite_all(writer->fd, writer->buffer, length)) {
        return bx_line_writer_set_error(writer);
    }
    return true;
}

bool bx_line_writer_write(struct bx_line_writer *writer, const void *data, size_t length) {
    if (!writer || (!data && length > 0u)) {
        errno = EINVAL;
        return false;
    }
    if (writer->error != 0) {
        return bx_line_writer_failed(writer);
    }
    if (length == 0u) {
        return true;
    }

    if (writer->capacity == 0u || length > writer->capacity) {
        if (!bx_line_writer_flush(writer)) {
            return false;
        }
        if (!bx_xwrite_all(writer->fd, data, length)) {
            return bx_line_writer_set_error(writer);
        }
        return true;
    }

    if (length > writer->capacity - writer->length && !bx_line_writer_flush(writer)) {
        return false;
    }

    memcpy(writer->buffer + writer->length, data, length);
    writer->length += length;
    return true;
}

bool bx_line_writer_putc(struct bx_line_writer *writer, char ch) {
    return bx_line_writer_write(writer, &ch, 1u);
}

bool bx_line_writer_puts(struct bx_line_writer *writer, const char *text) {
    if (!text) {
        errno = EINVAL;
        return false;
    }
    return bx_line_writer_write(writer, text, strlen(text));
}

bool bx_line_writer_put_line(struct bx_line_writer *writer, const char *text) {
    if (!text) {
        errno = EINVAL;
        return false;
    }
    return bx_line_writer_put_line_len(writer, text, strlen(text));
}

bool bx_line_writer_put_line_len(struct bx_line_writer *writer, const char *text, size_t length) {
    return bx_line_writer_write(writer, text, length) && bx_line_writer_putc(writer, '\n');
}

int bx_line_writer_error(const struct bx_line_writer *writer) {
    return writer ? writer->error : EINVAL;
}
