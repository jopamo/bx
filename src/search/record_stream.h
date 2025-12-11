#ifndef BX_SEARCH_RECORD_STREAM_H
#define BX_SEARCH_RECORD_STREAM_H

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

struct bx_record_stream {
    char *record;
    size_t record_cap;
    char *io_buf;
    size_t io_cap;
};

void bx_record_stream_dispose(struct bx_record_stream *stream);
void bx_record_stream_prepare_file(FILE *f, struct bx_record_stream *stream);
bool bx_record_stream_probe_binary_prefix(FILE *f, bool *is_binary_out);
ssize_t bx_record_stream_read(FILE *f, struct bx_record_stream *stream, char delimiter);

#endif
