#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "lib/line_writer.h"
#include "lib/output_profile_counter.h"
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

static bool bx_line_writer_xwrite_all_profiled(struct bx_line_writer *writer,
                                               const void *data,
                                               size_t count) {
    const unsigned char *p = (const unsigned char *)data;
    size_t done = 0u;

    while (done < count) {
        size_t remaining = count - done;
        ssize_t nwritten = write(writer->fd, p + done, remaining);
        if (nwritten < 0) {
            if (errno == EINTR) {
                bx_output_profile_note_retry(writer->profile);
                continue;
            }
            if (errno == EPIPE) {
                bx_output_profile_note_epipe(writer->profile);
            }
            return false;
        }
        if (nwritten == 0) {
            errno = EIO;
            return false;
        }
        bx_output_profile_note_bytes(writer->profile, (uint_fast64_t)nwritten);
        if ((size_t)nwritten < remaining) {
            bx_output_profile_note_short_write(writer->profile);
        }
        done += (size_t)nwritten;
    }

    return true;
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
    writer->profile = NULL;
}

void bx_line_writer_set_profile(struct bx_line_writer *writer, struct bx_output_profile_sink *profile) {
    if (!writer) {
        return;
    }
    writer->profile = profile;
}

bool bx_line_writer_flush(struct bx_line_writer *writer) {
    if (!writer) {
        errno = EINVAL;
        return false;
    }
    if (writer->error != 0) {
        return bx_line_writer_failed(writer);
    }
    if (writer->profile != NULL) {
        bx_output_profile_note_flush(writer->profile);
    }
    if (writer->length == 0u) {
        return true;
    }

    size_t length = writer->length;
    writer->length = 0u;
    if (writer->profile != NULL && bx_output_profile_sink_enabled(writer->profile)) {
        if (!bx_line_writer_xwrite_all_profiled(writer, writer->buffer, length)) {
            return bx_line_writer_set_error(writer);
        }
    } else if (!bx_xwrite_all(writer->fd, writer->buffer, length)) {
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
        if (writer->profile != NULL && bx_output_profile_sink_enabled(writer->profile)) {
            if (!bx_line_writer_xwrite_all_profiled(writer, data, length)) {
                return bx_line_writer_set_error(writer);
            }
        } else if (!bx_xwrite_all(writer->fd, data, length)) {
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
