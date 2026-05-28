#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dev_counters.h"
#include "record_stream.h"

#define BX_RECORD_STREAM_IO_CAP 65536u
#define BX_RECORD_STREAM_BINARY_PROBE_CAP 1024u
#define BX_RECORD_STREAM_READ_CHUNK_CAP 8192u
#define BX_RECORD_STREAM_SPECIAL_RECORD_LIMIT (16u * 1024u * 1024u)

static size_t bx_record_stream_pending_available(const struct bx_record_stream *stream) {
    if (!stream || stream->pending_len < stream->pending_off)
        return 0u;
    return stream->pending_len - stream->pending_off;
}

static void bx_record_stream_reset_pending(struct bx_record_stream *stream) {
    if (!stream)
        return;
    stream->pending_off = 0u;
    stream->pending_len = 0u;
}

static void bx_record_stream_compact_pending(struct bx_record_stream *stream) {
    size_t available;

    if (!stream || stream->pending_off == 0u)
        return;

    available = bx_record_stream_pending_available(stream);
    if (available > 0u)
        memmove(stream->pending, stream->pending + stream->pending_off, available);
    stream->pending_off = 0u;
    stream->pending_len = available;
}

static bool bx_record_stream_reserve_pending(struct bx_record_stream *stream, size_t needed) {
    unsigned char *tmp;
    size_t new_cap;

    if (!stream)
        return false;
    if (stream->pending_cap >= needed)
        return true;

    new_cap = stream->pending_cap == 0u ? BX_RECORD_STREAM_BINARY_PROBE_CAP : stream->pending_cap;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2u))
            return false;
        new_cap *= 2u;
    }

    tmp = realloc(stream->pending, new_cap);
    if (!tmp)
        return false;

    stream->pending = tmp;
    stream->pending_cap = new_cap;
    return true;
}

static bool bx_record_stream_append_pending(struct bx_record_stream *stream,
                                            const unsigned char *data,
                                            size_t len) {
    size_t available;

    if (!stream || len == 0u)
        return true;

    bx_record_stream_compact_pending(stream);
    available = bx_record_stream_pending_available(stream);
    if (!bx_record_stream_reserve_pending(stream, available + len))
        return false;

    memcpy(stream->pending + stream->pending_len, data, len);
    stream->pending_len += len;
    return true;
}

static bool bx_record_stream_reserve_record(struct bx_record_stream *stream, size_t needed) {
    char *tmp;
    size_t new_cap;

    if (!stream)
        return false;
    if (needed > SIZE_MAX - 1u)
        return false;
    if (stream->record_cap >= needed + 1u)
        return true;

    new_cap = stream->record_cap == 0u ? 256u : stream->record_cap;
    while (new_cap < needed + 1u) {
        if (new_cap > (SIZE_MAX / 2u))
            return false;
        new_cap *= 2u;
    }

    tmp = realloc(stream->record, new_cap);
    if (!tmp)
        return false;

    stream->record = tmp;
    stream->record_cap = new_cap;
    return true;
}

static bool bx_record_stream_append_record(struct bx_record_stream *stream,
                                           size_t *len_io,
                                           const unsigned char *data,
                                           size_t len) {
    size_t current_len;
    size_t needed;

    if (!stream || !len_io)
        return false;
    if (len == 0u)
        return true;

    current_len = *len_io;
    if (current_len > SIZE_MAX - len)
        return false;
    needed = current_len + len;
    if (stream->record_limit > 0u && needed > stream->record_limit) {
        stream->errnum = EOVERFLOW;
        return false;
    }
    if (!bx_record_stream_reserve_record(stream, needed)) {
        stream->errnum = ENOMEM;
        return false;
    }

    memcpy((unsigned char *)stream->record + current_len, data, len);
    *len_io = needed;
    stream->record[needed] = '\0';
    return true;
}

static size_t bx_record_stream_configure_limit(FILE *f) {
    struct stat st;
    int fd;

    if (!f)
        return 0u;

    fd = fileno(f);
    if (fd < 0)
        return 0u;
    bx_search_dev_counters_note_content_fstat_call();
    if (fstat(fd, &st) != 0)
        return 0u;
    if (S_ISREG(st.st_mode))
        return 0u;
    return BX_RECORD_STREAM_SPECIAL_RECORD_LIMIT;
}

static size_t bx_record_stream_fill_pending(FILE *f,
                                            struct bx_record_stream *stream,
                                            size_t target) {
    size_t available;

    if (!f || !stream)
        return 0u;

    available = bx_record_stream_pending_available(stream);
    while (available < target) {
        size_t nread;

        bx_record_stream_compact_pending(stream);
        available = bx_record_stream_pending_available(stream);
        if (!bx_record_stream_reserve_pending(stream, target)) {
            stream->errnum = ENOMEM;
            return available;
        }

        nread = fread(stream->pending + stream->pending_len, 1u, target - available, f);
        stream->pending_len += nread;
        bx_search_dev_counters_note_content_read(nread);
        available += nread;
        if (nread == 0u) {
            if (ferror(f))
                stream->errnum = errno ? errno : EIO;
            break;
        }
    }

    return available;
}

void bx_record_stream_dispose(struct bx_record_stream *stream) {
    if (!stream)
        return;

    free(stream->record);
    free(stream->io_buf);
    free(stream->pending);
    stream->record = NULL;
    stream->record_cap = 0;
    stream->io_buf = NULL;
    stream->io_cap = 0;
    stream->pending = NULL;
    stream->pending_off = 0u;
    stream->pending_len = 0u;
    stream->pending_cap = 0u;
    stream->record_limit = 0u;
    stream->errnum = 0;
}

void bx_record_stream_prepare_file(FILE *f, struct bx_record_stream *stream) {
    if (!f || !stream)
        return;

    stream->errnum = 0;
    bx_record_stream_reset_pending(stream);
    stream->record_limit = bx_record_stream_configure_limit(f);

    if (!stream->io_buf) {
        stream->io_cap = BX_RECORD_STREAM_IO_CAP;
        stream->io_buf = malloc(stream->io_cap);
        if (!stream->io_buf) {
            stream->io_cap = 0;
            return;
        }
    }

    setvbuf(f, stream->io_buf, _IOFBF, stream->io_cap);
}

bool bx_record_stream_probe_binary_prefix(FILE *f,
                                          struct bx_record_stream *stream,
                                          bool *is_binary_out) {
    int fd;
    struct stat st;
    size_t available;

    if (!f || !stream)
        return false;

    stream->errnum = 0;
    fd = fileno(f);
    if (fd >= 0) {
        bx_search_dev_counters_note_content_fstat_call();
    }
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        unsigned char buf[BX_RECORD_STREAM_BINARY_PROBE_CAP];
        ssize_t nread = pread(fd, buf, sizeof(buf), 0);

        if (nread < 0) {
            stream->errnum = errno ? errno : EIO;
            return false;
        }
        bx_search_dev_counters_note_bytes_read((size_t)nread);
        bx_search_dev_counters_note_content_pread((size_t)nread);
        bx_search_dev_counters_note_prefix_pread((size_t)nread);
        bx_search_dev_counters_note_prefix_bytes_rescanned((size_t)nread);
        if (is_binary_out) {
            bx_search_dev_counters_note_binary_prefix_check();
            *is_binary_out = memchr(buf, '\0', (size_t)nread) != NULL;
        }
        return true;
    }

    available = bx_record_stream_fill_pending(f, stream, BX_RECORD_STREAM_BINARY_PROBE_CAP);
    if (stream->errnum != 0)
        return false;

    if (is_binary_out) {
        bx_search_dev_counters_note_binary_prefix_check();
        *is_binary_out =
            memchr(stream->pending + stream->pending_off, '\0', available) != NULL;
    }
    return true;
}

size_t bx_record_stream_read_chunk(FILE *f,
                                   struct bx_record_stream *stream,
                                   unsigned char *buf,
                                   size_t cap) {
    size_t copied = 0u;
    size_t available;

    if (!f || !stream || !buf || cap == 0u)
        return 0u;

    available = bx_record_stream_pending_available(stream);
    if (available > 0u) {
        copied = available < cap ? available : cap;
        memcpy(buf, stream->pending + stream->pending_off, copied);
        stream->pending_off += copied;
        if (stream->pending_off == stream->pending_len)
            bx_record_stream_reset_pending(stream);
    }
    if (copied == cap)
        return copied;

    {
        size_t nread = fread(buf + copied, 1u, cap - copied, f);

        copied += nread;
        bx_search_dev_counters_note_content_read(nread);
        if (nread == 0u && ferror(f))
            stream->errnum = errno ? errno : EIO;
    }
    return copied;
}

bool bx_record_stream_had_error(const struct bx_record_stream *stream) {
    return stream && stream->errnum != 0;
}

int bx_record_stream_error(const struct bx_record_stream *stream) {
    return stream ? stream->errnum : 0;
}

size_t bx_record_stream_record_limit(const struct bx_record_stream *stream) {
    return stream ? stream->record_limit : 0u;
}

size_t bx_record_stream_default_record_limit(void) {
    return BX_RECORD_STREAM_SPECIAL_RECORD_LIMIT;
}

ssize_t bx_record_stream_read(FILE *f, struct bx_record_stream *stream, char delimiter) {
    size_t len = 0u;

    if (!f || !stream)
        return -1;

    stream->errnum = 0;
    if (!bx_record_stream_reserve_record(stream, 0u)) {
        stream->errnum = ENOMEM;
        return -1;
    }
    stream->record[0] = '\0';

    for (;;) {
        size_t available = bx_record_stream_pending_available(stream);
        if (available > 0u) {
            const unsigned char *pending = stream->pending + stream->pending_off;
            const unsigned char *hit = memchr(pending, (unsigned char)delimiter, available);
            size_t take = hit ? (size_t)(hit - pending) + 1u : available;

            if (!bx_record_stream_append_record(stream, &len, pending, take))
                return -1;
            stream->pending_off += take;
            if (stream->pending_off == stream->pending_len)
                bx_record_stream_reset_pending(stream);
            if (hit)
                break;
        }

        {
            unsigned char chunk[BX_RECORD_STREAM_READ_CHUNK_CAP];
            size_t nread = fread(chunk, 1u, sizeof(chunk), f);

            bx_search_dev_counters_note_content_read(nread);
            if (nread == 0u) {
                if (ferror(f)) {
                    stream->errnum = errno ? errno : EIO;
                    return -1;
                }
                if (len == 0u)
                    return -1;
                break;
            }

            {
                const unsigned char *hit = memchr(chunk, (unsigned char)delimiter, nread);
                size_t take = hit ? (size_t)(hit - chunk) + 1u : nread;
                size_t remain = nread - take;

                if (!bx_record_stream_append_record(stream, &len, chunk, take))
                    return -1;
                if (remain > 0u &&
                    !bx_record_stream_append_pending(stream, chunk + take, remain)) {
                    stream->errnum = ENOMEM;
                    return -1;
                }
                if (hit)
                    break;
            }
        }
    }

    bx_search_dev_counters_note_record_materialized();
    return (ssize_t)len;
}
