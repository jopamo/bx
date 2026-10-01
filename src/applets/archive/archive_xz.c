#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/archive/archive_xz.h"
#include "bx/libbx.h"

#if BX_HAVE_LIBLZMA
#include <lzma.h>
#endif

#define BX_ARCHIVE_XZ_IO_CHUNK 8192u

struct bx_archive_xz_reader {
#if BX_HAVE_LIBLZMA
    lzma_stream stream;
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

bool bx_archive_xz_reader_open(struct bx_archive_xz_reader** reader_out, struct bx_diag_ctx* diag) {
#if BX_HAVE_LIBLZMA
    if (!reader_out) {
        bx_diag(diag, "invalid xz reader configuration");
        return false;
    }
    *reader_out = NULL;
    struct bx_archive_xz_reader* reader = xmalloc(sizeof(*reader));
    memset(reader, 0, sizeof(*reader));
    reader->stream = (lzma_stream)LZMA_STREAM_INIT;
    lzma_ret rc = lzma_stream_decoder(&reader->stream, UINT64_MAX, LZMA_CONCATENATED);
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
    bx_diag(diag, "xz support is unavailable in this build");
    return false;
#endif
}

enum bx_archive_decode_result bx_archive_xz_reader_decode(struct bx_archive_xz_reader* reader, struct bx_archive_decode_chunk* chunk) {
#if BX_HAVE_LIBLZMA
    chunk->error_detail = NULL;
    reader->stream.next_in = chunk->input;
    reader->stream.avail_in = chunk->input_size;
    reader->stream.next_out = chunk->output;
    reader->stream.avail_out = chunk->output_size;
    lzma_ret rc = lzma_code(&reader->stream, chunk->input_eof ? LZMA_FINISH : LZMA_RUN);
    chunk->input_used = chunk->input_size - reader->stream.avail_in;
    chunk->output_used = chunk->output_size - reader->stream.avail_out;
    reader->stream.next_in = reader->stream.next_out = NULL;
    reader->stream.avail_in = reader->stream.avail_out = 0u;
    if (rc == LZMA_STREAM_END)
        return BX_ARCHIVE_DECODE_END;
    if (rc == LZMA_OK && chunk->input_eof && !chunk->input_used && !chunk->output_used)
        rc = LZMA_BUF_ERROR;
    if (rc != LZMA_OK) {
        chunk->error_detail = bx_archive_xz_ret_detail(rc);
        return BX_ARCHIVE_DECODE_ERROR;
    }
    return BX_ARCHIVE_DECODE_MORE;
#else
    (void)reader;
    chunk->error_detail = "support is unavailable in this build";
    return BX_ARCHIVE_DECODE_ERROR;
#endif
}

void bx_archive_xz_reader_close(struct bx_archive_xz_reader* reader) {
#if BX_HAVE_LIBLZMA
    if (!reader)
        return;
    if (reader->stream_initialized)
        lzma_end(&reader->stream);
#endif
    free(reader);
}
