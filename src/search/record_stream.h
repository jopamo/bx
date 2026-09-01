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
    unsigned char *pending;
    size_t pending_off;
    size_t pending_len;
    size_t pending_cap;
    size_t record_limit;
    int errnum;
};

void bx_record_stream_dispose(struct bx_record_stream *stream);
void bx_record_stream_begin(FILE *f, struct bx_record_stream *stream);
void bx_record_stream_prepare_file(FILE *f, struct bx_record_stream *stream);
bool bx_record_stream_probe_binary_prefix(FILE *f,
                                          struct bx_record_stream *stream,
                                          bool *is_binary_out);
ssize_t bx_record_stream_read(FILE *f, struct bx_record_stream *stream, char delimiter);
ssize_t bx_record_stream_read_until(FILE *f,
                                    struct bx_record_stream *stream,
                                    char delimiter,
                                    bool stop_enabled,
                                    unsigned char stop_byte,
                                    bool *stopped_out);
ssize_t bx_record_stream_read_live(FILE *f, struct bx_record_stream *stream, char delimiter);
size_t bx_record_stream_read_chunk(FILE *f,
                                   struct bx_record_stream *stream,
                                   unsigned char *buf,
                                   size_t cap);
bool bx_record_stream_had_error(const struct bx_record_stream *stream);
int bx_record_stream_error(const struct bx_record_stream *stream);
size_t bx_record_stream_record_limit(const struct bx_record_stream *stream);
size_t bx_record_stream_default_record_limit(void);

#endif
