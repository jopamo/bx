#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dev_counters.h"
#include "record_stream.h"

#define BX_RECORD_STREAM_IO_CAP 65536u

void bx_record_stream_dispose(struct bx_record_stream *stream) {
    if (!stream)
        return;

    free(stream->record);
    free(stream->io_buf);
    stream->record = NULL;
    stream->record_cap = 0;
    stream->io_buf = NULL;
    stream->io_cap = 0;
}

void bx_record_stream_prepare_file(FILE *f, struct bx_record_stream *stream) {
    if (!f || !stream)
        return;

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

bool bx_record_stream_probe_binary_prefix(FILE *f, bool *is_binary_out) {
    unsigned char buf[1024];
    int fd = fileno(f);
    if (fd < 0)
        return false;

    ssize_t n = pread(fd, buf, sizeof(buf), 0);
    if (n < 0)
        return false;
    bx_search_dev_counters_note_bytes_read((size_t)n);

    if (is_binary_out)
        *is_binary_out = (memchr(buf, '\0', (size_t)n) != NULL);
    return true;
}

ssize_t bx_record_stream_read(FILE *f, struct bx_record_stream *stream, char delimiter) {
    if (!stream)
        return -1;

    ssize_t nread = getdelim(&stream->record, &stream->record_cap, delimiter, f);
    if (nread > 0) {
        bx_search_dev_counters_note_bytes_read((size_t)nread);
        bx_search_dev_counters_note_record_materialized();
    }
    return nread;
}
