#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets/archive/archive_xz.h"
#include "bx/libbx.h"

#if BX_HAVE_LIBLZMA
#include <lzma.h>
#endif

#define BX_ARCHIVE_XZ_IO_CHUNK 8192u
#define BX_ARCHIVE_XZ_READER_IN_CHUNK 8192u
#define BX_ARCHIVE_XZ_READER_OUT_CHUNK 8192u

struct bx_archive_xz_reader {
#if BX_HAVE_LIBLZMA
    int fd;
    lzma_stream stream;
    unsigned char inbuf[BX_ARCHIVE_XZ_READER_IN_CHUNK];
    unsigned char outbuf[BX_ARCHIVE_XZ_READER_OUT_CHUNK];
    size_t out_pos;
    size_t out_len;
    bool input_eof;
    bool finished;
    bool stream_initialized;
#else
    int unused;
#endif
};

#if BX_HAVE_LIBLZMA
static const char* bx_archive_xz_ret_detail(lzma_ret rc) {
    switch (rc) {
        case LZMA_OK:
            return NULL;
        case LZMA_STREAM_END:
            return NULL;
        case LZMA_MEM_ERROR:
            return "memory allocation failed";
        case LZMA_FORMAT_ERROR:
            return "file format not recognized";
        case LZMA_OPTIONS_ERROR:
            return "unsupported stream options";
        case LZMA_DATA_ERROR:
            return "compressed data is corrupt";
        case LZMA_BUF_ERROR:
            return "compressed data is truncated";
        case LZMA_PROG_ERROR:
            return "internal codec error";
        default:
            return "codec error";
    }
}

static void bx_archive_xz_diag_failed(const char* action, lzma_ret rc, struct bx_diag_ctx* diag) {
    const char* detail = bx_archive_xz_ret_detail(rc);

    bx_diag(diag,
            "xz %s failed%s%s",
            action,
            detail != NULL ? ": " : "",
            detail != NULL ? detail : "");
}

static bool bx_archive_xz_write_output(const struct bx_archive_xz_stream_sink* sink,
                                       const unsigned char* data,
                                       size_t len,
                                       struct bx_diag_ctx* diag) {
    if (len == 0u) {
        return true;
    }
    if (!sink->write(sink->user, data, len)) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_archive_xz_run_buffer_filter(const unsigned char* input,
                                            size_t input_len,
                                            struct bx_archive_buffer* output,
                                            bool decompress,
                                            struct bx_diag_ctx* diag) {
    lzma_stream stream = LZMA_STREAM_INIT;
    size_t input_pos = 0u;
    bool ok = false;
    lzma_ret rc;

    rc = decompress
        ? lzma_stream_decoder(&stream, UINT64_MAX, LZMA_CONCATENATED)
        : lzma_easy_encoder(&stream, LZMA_PRESET_DEFAULT, LZMA_CHECK_CRC64);
    if (rc != LZMA_OK) {
        bx_archive_xz_diag_failed(decompress ? "decompression" : "compression", rc, diag);
        return false;
    }

    for (;;) {
        unsigned char outbuf[BX_ARCHIVE_XZ_IO_CHUNK];
        lzma_action action = LZMA_RUN;
        size_t produced;

        if (stream.avail_in == 0u && input_pos < input_len) {
            size_t chunk = input_len - input_pos;

            stream.next_in = input + input_pos;
            stream.avail_in = chunk;
            input_pos += chunk;
        }
        if (input_pos == input_len) {
            action = LZMA_FINISH;
        }

        stream.next_out = outbuf;
        stream.avail_out = sizeof(outbuf);
        rc = lzma_code(&stream, action);
        produced = sizeof(outbuf) - stream.avail_out;
        if (produced > 0u && !bx_archive_buffer_append(output, outbuf, produced)) {
            lzma_end(&stream);
            bx_diag(diag, "buffer growth failed: %s", strerror(errno));
            return false;
        }
        if (rc == LZMA_STREAM_END) {
            ok = true;
            break;
        }
        if (rc != LZMA_OK) {
            bx_archive_xz_diag_failed(decompress ? "decompression" : "compression", rc, diag);
            break;
        }
        if (action == LZMA_FINISH && produced == 0u && stream.avail_in == 0u) {
            bx_archive_xz_diag_failed(decompress ? "decompression" : "compression",
                                      LZMA_BUF_ERROR,
                                      diag);
            break;
        }
    }

    lzma_end(&stream);
    return ok;
}

struct bx_archive_xz_filter_stream_state {
    const struct bx_archive_xz_stream_sink* output_sink;
    struct bx_diag_ctx* diag;
    lzma_stream stream;
    bool stream_initialized;
};

static bool bx_archive_xz_filter_stream_feed(struct bx_archive_xz_filter_stream_state* state,
                                             const unsigned char* data,
                                             size_t len) {
    while (len > 0u) {
        unsigned char outbuf[BX_ARCHIVE_XZ_IO_CHUNK];
        size_t chunk = len;
        size_t produced;
        lzma_ret rc;

        state->stream.next_in = data;
        state->stream.avail_in = chunk;
        data += chunk;
        len -= chunk;

        while (state->stream.avail_in > 0u) {
            state->stream.next_out = outbuf;
            state->stream.avail_out = sizeof(outbuf);
            rc = lzma_code(&state->stream, LZMA_RUN);
            if (rc != LZMA_OK) {
                bx_archive_xz_diag_failed("compression", rc, state->diag);
                return false;
            }
            produced = sizeof(outbuf) - state->stream.avail_out;
            if (!bx_archive_xz_write_output(state->output_sink, outbuf, produced, state->diag)) {
                return false;
            }
        }
    }

    return true;
}

static bool bx_archive_xz_filter_stream_finish(struct bx_archive_xz_filter_stream_state* state) {
    for (;;) {
        unsigned char outbuf[BX_ARCHIVE_XZ_IO_CHUNK];
        size_t produced;
        lzma_ret rc;

        state->stream.next_out = outbuf;
        state->stream.avail_out = sizeof(outbuf);
        rc = lzma_code(&state->stream, LZMA_FINISH);
        produced = sizeof(outbuf) - state->stream.avail_out;
        if (!bx_archive_xz_write_output(state->output_sink, outbuf, produced, state->diag)) {
            return false;
        }
        if (rc == LZMA_STREAM_END) {
            return true;
        }
        if (rc != LZMA_OK) {
            bx_archive_xz_diag_failed("compression", rc, state->diag);
            return false;
        }
    }
}

static bool bx_archive_xz_filter_stream_input_write(void* user, const void* data, size_t len) {
    struct bx_archive_xz_filter_stream_state* state = user;
    return bx_archive_xz_filter_stream_feed(state, data, len);
}

static bool bx_archive_xz_reader_fill_output(struct bx_archive_xz_reader* reader,
                                             struct bx_diag_ctx* diag) {
    while (reader->out_pos == reader->out_len && !reader->finished) {
        lzma_action action = LZMA_RUN;
        size_t produced;
        lzma_ret rc;

        if (reader->stream.avail_in == 0u && !reader->input_eof) {
            ssize_t nread = read(reader->fd, reader->inbuf, sizeof(reader->inbuf));

            if (nread < 0) {
                bx_diag(diag, "read error: %s", strerror(errno));
                return false;
            }
            if (nread == 0) {
                reader->input_eof = true;
            }
            reader->stream.next_in = reader->inbuf;
            reader->stream.avail_in = nread > 0 ? (size_t)nread : 0u;
        }
        if (reader->input_eof) {
            action = LZMA_FINISH;
        }

        reader->stream.next_out = reader->outbuf;
        reader->stream.avail_out = sizeof(reader->outbuf);
        rc = lzma_code(&reader->stream, action);
        produced = sizeof(reader->outbuf) - reader->stream.avail_out;
        reader->out_pos = 0u;
        reader->out_len = produced;

        if (rc == LZMA_STREAM_END) {
            reader->finished = true;
            return true;
        }
        if (rc != LZMA_OK) {
            bx_archive_xz_diag_failed("decompression", rc, diag);
            return false;
        }
        if (produced > 0u) {
            return true;
        }
        if (reader->input_eof && reader->stream.avail_in == 0u) {
            bx_archive_xz_diag_failed("decompression", LZMA_BUF_ERROR, diag);
            return false;
        }
    }

    return true;
}
#endif

bool bx_archive_run_xz_filter(const struct bx_archive_buffer* input,
                              struct bx_archive_buffer* output,
                              bool decompress,
                              struct bx_diag_ctx* diag) {
#if BX_HAVE_LIBLZMA
    return bx_archive_xz_run_buffer_filter(input->data, input->len, output, decompress, diag);
#else
    (void)input;
    (void)output;
    (void)decompress;
    bx_diag(diag, "xz support is unavailable in this build");
    return false;
#endif
}

bool bx_archive_run_xz_filter_stream(bx_archive_xz_stream_producer_fn producer,
                                     void* producer_user,
                                     const struct bx_archive_xz_stream_sink* output_sink,
                                     struct bx_diag_ctx* diag) {
#if BX_HAVE_LIBLZMA
    struct bx_archive_xz_filter_stream_state state;
    struct bx_archive_xz_stream_sink input_sink;
    lzma_ret rc;
    bool ok = false;

    if (producer == NULL || output_sink == NULL || output_sink->write == NULL) {
        bx_diag(diag, "invalid xz stream configuration");
        return false;
    }

    memset(&state, 0, sizeof(state));
    state.output_sink = output_sink;
    state.diag = diag;
    state.stream = (lzma_stream)LZMA_STREAM_INIT;

    rc = lzma_easy_encoder(&state.stream, LZMA_PRESET_DEFAULT, LZMA_CHECK_CRC64);
    if (rc != LZMA_OK) {
        bx_archive_xz_diag_failed("compression", rc, diag);
        return false;
    }
    state.stream_initialized = true;

    input_sink.user = &state;
    input_sink.write = bx_archive_xz_filter_stream_input_write;
    if (!producer(producer_user, &input_sink, diag)) {
        goto out;
    }

    ok = bx_archive_xz_filter_stream_finish(&state);

out:
    if (state.stream_initialized) {
        lzma_end(&state.stream);
    }
    return ok;
#else
    (void)producer;
    (void)producer_user;
    (void)output_sink;
    bx_diag(diag, "xz support is unavailable in this build");
    return false;
#endif
}

bool bx_archive_xz_reader_open(struct bx_archive_xz_reader** reader_out,
                               int fd,
                               struct bx_diag_ctx* diag) {
#if BX_HAVE_LIBLZMA
    struct bx_archive_xz_reader* reader;
    lzma_ret rc;

    if (reader_out == NULL) {
        bx_diag(diag, "invalid xz reader configuration");
        return false;
    }
    *reader_out = NULL;

    reader = xmalloc(sizeof(*reader));
    memset(reader, 0, sizeof(*reader));
    reader->fd = fd;
    reader->stream = (lzma_stream)LZMA_STREAM_INIT;

    rc = lzma_stream_decoder(&reader->stream, UINT64_MAX, LZMA_CONCATENATED);
    if (rc != LZMA_OK) {
        free(reader);
        bx_archive_xz_diag_failed("decompression", rc, diag);
        return false;
    }
    reader->stream_initialized = true;
    *reader_out = reader;
    return true;
#else
    (void)reader_out;
    (void)fd;
    bx_diag(diag, "xz support is unavailable in this build");
    return false;
#endif
}

bool bx_archive_xz_reader_read_some(struct bx_archive_xz_reader* reader,
                                    unsigned char* buffer,
                                    size_t len,
                                    size_t* nread_out,
                                    struct bx_diag_ctx* diag) {
#if BX_HAVE_LIBLZMA
    size_t total = 0u;

    while (total < len) {
        size_t chunk;

        if (reader->out_pos == reader->out_len && !reader->finished) {
            if (!bx_archive_xz_reader_fill_output(reader, diag)) {
                return false;
            }
        }
        if (reader->out_pos == reader->out_len) {
            break;
        }

        chunk = len - total;
        if (chunk > reader->out_len - reader->out_pos) {
            chunk = reader->out_len - reader->out_pos;
        }
        memcpy(buffer + total, reader->outbuf + reader->out_pos, chunk);
        reader->out_pos += chunk;
        total += chunk;
    }

    *nread_out = total;
    return true;
#else
    (void)reader;
    (void)buffer;
    (void)len;
    (void)nread_out;
    bx_diag(diag, "xz support is unavailable in this build");
    return false;
#endif
}

void bx_archive_xz_reader_close(struct bx_archive_xz_reader* reader) {
#if BX_HAVE_LIBLZMA
    if (reader == NULL) {
        return;
    }
    if (reader->stream_initialized) {
        lzma_end(&reader->stream);
    }
    if (reader->fd >= 0) {
        close(reader->fd);
    }
    free(reader);
#else
    free(reader);
#endif
}
