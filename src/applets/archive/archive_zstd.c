#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets/archive/archive_zstd.h"
#include "bx/libbx.h"

#if BX_HAVE_LIBZSTD
#include <zstd.h>
#endif

struct bx_archive_zstd_reader {
#if BX_HAVE_LIBZSTD
    int fd;
    ZSTD_DStream* stream;
    void* inbuf;
    size_t inbuf_size;
    size_t inbuf_pos;
    size_t inbuf_len;
    void* outbuf;
    size_t outbuf_size;
    size_t out_pos;
    size_t out_len;
    bool input_eof;
    bool finished;
#else
    int unused;
#endif
};

#if BX_HAVE_LIBZSTD
static void bx_archive_zstd_diag_failed(const char* action,
                                        size_t rc,
                                        const char* fallback,
                                        struct bx_diag_ctx* diag) {
    const char* detail = fallback;

    if (ZSTD_isError(rc)) {
        detail = ZSTD_getErrorName(rc);
    }
    bx_diag(diag,
            "zstd %s failed%s%s",
            action,
            detail != NULL ? ": " : "",
            detail != NULL ? detail : "");
}

static bool bx_archive_zstd_write_output(const struct bx_archive_zstd_stream_sink* sink,
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

static bool bx_archive_zstd_run_buffer_encode(const unsigned char* input,
                                              size_t input_len,
                                              struct bx_archive_buffer* output,
                                              struct bx_diag_ctx* diag) {
    ZSTD_CStream* stream = NULL;
    size_t outbuf_size = ZSTD_CStreamOutSize();
    unsigned char* outbuf = NULL;
    size_t rc;
    bool ok = false;

    stream = ZSTD_createCStream();
    if (stream == NULL) {
        bx_archive_zstd_diag_failed("compression", (size_t)-1, "stream allocation failed", diag);
        return false;
    }
    rc = ZSTD_initCStream(stream, ZSTD_CLEVEL_DEFAULT);
    if (ZSTD_isError(rc)) {
        bx_archive_zstd_diag_failed("compression", rc, NULL, diag);
        goto out;
    }
    outbuf = xmalloc(outbuf_size);

    {
        ZSTD_inBuffer in = {
            .src = input,
            .size = input_len,
            .pos = 0u,
        };

        while (in.pos < in.size) {
            ZSTD_outBuffer out = {
                .dst = outbuf,
                .size = outbuf_size,
                .pos = 0u,
            };

            rc = ZSTD_compressStream2(stream, &out, &in, ZSTD_e_continue);
            if (ZSTD_isError(rc)) {
                bx_archive_zstd_diag_failed("compression", rc, NULL, diag);
                goto out;
            }
            if (out.pos > 0u && !bx_archive_buffer_append(output, outbuf, out.pos)) {
                bx_diag(diag, "buffer growth failed: %s", strerror(errno));
                goto out;
            }
        }
    }

    for (;;) {
        ZSTD_outBuffer out = {
            .dst = outbuf,
            .size = outbuf_size,
            .pos = 0u,
        };

        rc = ZSTD_compressStream2(stream, &out, &(ZSTD_inBuffer){0}, ZSTD_e_end);
        if (ZSTD_isError(rc)) {
            bx_archive_zstd_diag_failed("compression", rc, NULL, diag);
            goto out;
        }
        if (out.pos > 0u && !bx_archive_buffer_append(output, outbuf, out.pos)) {
            bx_diag(diag, "buffer growth failed: %s", strerror(errno));
            goto out;
        }
        if (rc == 0u) {
            ok = true;
            break;
        }
    }

out:
    free(outbuf);
    ZSTD_freeCStream(stream);
    return ok;
}

static bool bx_archive_zstd_run_buffer_decode(const unsigned char* input,
                                              size_t input_len,
                                              struct bx_archive_buffer* output,
                                              struct bx_diag_ctx* diag) {
    ZSTD_DStream* stream = NULL;
    size_t outbuf_size = ZSTD_DStreamOutSize();
    unsigned char* outbuf = NULL;
    bool ok = false;
    size_t rc;

    stream = ZSTD_createDStream();
    if (stream == NULL) {
        bx_archive_zstd_diag_failed("decompression", (size_t)-1, "stream allocation failed", diag);
        return false;
    }
    rc = ZSTD_initDStream(stream);
    if (ZSTD_isError(rc)) {
        bx_archive_zstd_diag_failed("decompression", rc, NULL, diag);
        goto out;
    }
    outbuf = xmalloc(outbuf_size);

    {
        ZSTD_inBuffer in = {
            .src = input,
            .size = input_len,
            .pos = 0u,
        };

        while (true) {
            ZSTD_outBuffer out = {
                .dst = outbuf,
                .size = outbuf_size,
                .pos = 0u,
            };

            rc = ZSTD_decompressStream(stream, &out, &in);
            if (ZSTD_isError(rc)) {
                bx_archive_zstd_diag_failed("decompression", rc, NULL, diag);
                goto out;
            }
            if (out.pos > 0u && !bx_archive_buffer_append(output, outbuf, out.pos)) {
                bx_diag(diag, "buffer growth failed: %s", strerror(errno));
                goto out;
            }
            if (rc == 0u) {
                if (in.pos == in.size) {
                    ok = true;
                    break;
                }
                rc = ZSTD_initDStream(stream);
                if (ZSTD_isError(rc)) {
                    bx_archive_zstd_diag_failed("decompression", rc, NULL, diag);
                    goto out;
                }
                continue;
            }
            if (in.pos == in.size && out.pos == 0u) {
                bx_archive_zstd_diag_failed("decompression", (size_t)-1, "compressed data is truncated", diag);
                goto out;
            }
        }
    }

out:
    free(outbuf);
    ZSTD_freeDStream(stream);
    return ok;
}

struct bx_archive_zstd_filter_stream_state {
    const struct bx_archive_zstd_stream_sink* output_sink;
    struct bx_diag_ctx* diag;
    ZSTD_CStream* stream;
    void* outbuf;
    size_t outbuf_size;
};

static bool bx_archive_zstd_filter_stream_feed(struct bx_archive_zstd_filter_stream_state* state,
                                               const unsigned char* data,
                                               size_t len) {
    ZSTD_inBuffer in = {
        .src = data,
        .size = len,
        .pos = 0u,
    };

    while (in.pos < in.size) {
        ZSTD_outBuffer out = {
            .dst = state->outbuf,
            .size = state->outbuf_size,
            .pos = 0u,
        };
        size_t rc = ZSTD_compressStream2(state->stream, &out, &in, ZSTD_e_continue);

        if (ZSTD_isError(rc)) {
            bx_archive_zstd_diag_failed("compression", rc, NULL, state->diag);
            return false;
        }
        if (!bx_archive_zstd_write_output(state->output_sink, state->outbuf, out.pos, state->diag)) {
            return false;
        }
    }

    return true;
}

static bool bx_archive_zstd_filter_stream_finish(struct bx_archive_zstd_filter_stream_state* state) {
    for (;;) {
        ZSTD_outBuffer out = {
            .dst = state->outbuf,
            .size = state->outbuf_size,
            .pos = 0u,
        };
        size_t rc = ZSTD_compressStream2(state->stream, &out, &(ZSTD_inBuffer){0}, ZSTD_e_end);

        if (ZSTD_isError(rc)) {
            bx_archive_zstd_diag_failed("compression", rc, NULL, state->diag);
            return false;
        }
        if (!bx_archive_zstd_write_output(state->output_sink, state->outbuf, out.pos, state->diag)) {
            return false;
        }
        if (rc == 0u) {
            return true;
        }
    }
}

static bool bx_archive_zstd_filter_stream_input_write(void* user, const void* data, size_t len) {
    struct bx_archive_zstd_filter_stream_state* state = user;
    return bx_archive_zstd_filter_stream_feed(state, data, len);
}

static bool bx_archive_zstd_reader_fill_output(struct bx_archive_zstd_reader* reader,
                                               struct bx_diag_ctx* diag) {
    while (reader->out_pos == reader->out_len && !reader->finished) {
        ZSTD_inBuffer in;
        ZSTD_outBuffer out;
        size_t rc;

        if (reader->inbuf_pos == reader->inbuf_len && !reader->input_eof && reader->stream != NULL) {
            ssize_t nread = read(reader->fd, reader->inbuf, reader->inbuf_size);

            if (nread < 0) {
                bx_diag(diag, "read error: %s", strerror(errno));
                return false;
            }
            if (nread == 0) {
                reader->input_eof = true;
            }
            reader->inbuf_pos = 0u;
            reader->inbuf_len = nread > 0 ? (size_t)nread : 0u;
        }
        in.src = reader->inbuf;
        in.size = reader->inbuf_len;
        in.pos = reader->inbuf_pos;

        out.dst = reader->outbuf;
        out.size = reader->outbuf_size;
        out.pos = 0u;

        rc = ZSTD_decompressStream(reader->stream, &out, &in);
        if (ZSTD_isError(rc)) {
            bx_archive_zstd_diag_failed("decompression", rc, NULL, diag);
            return false;
        }
        reader->inbuf_pos = in.pos;
        reader->out_pos = 0u;
        reader->out_len = out.pos;

        if (rc == 0u) {
            if (reader->inbuf_pos == reader->inbuf_len && reader->input_eof) {
                reader->finished = true;
                return true;
            }
            if (reader->inbuf_pos == reader->inbuf_len && !reader->input_eof) {
                ssize_t nread = read(reader->fd, reader->inbuf, reader->inbuf_size);

                if (nread < 0) {
                    bx_diag(diag, "read error: %s", strerror(errno));
                    return false;
                }
                if (nread == 0) {
                    reader->input_eof = true;
                    reader->finished = true;
                    return true;
                }
                reader->inbuf_pos = 0u;
                reader->inbuf_len = (size_t)nread;
            }
            rc = ZSTD_initDStream(reader->stream);
            if (ZSTD_isError(rc)) {
                bx_archive_zstd_diag_failed("decompression", rc, NULL, diag);
                return false;
            }
            if (reader->out_len > 0u) {
                return true;
            }
            continue;
        }
        if (out.pos > 0u) {
            return true;
        }
        if (reader->input_eof && reader->inbuf_pos == reader->inbuf_len) {
            bx_archive_zstd_diag_failed("decompression", (size_t)-1, "compressed data is truncated", diag);
            return false;
        }
    }

    return true;
}
#endif

bool bx_archive_run_zstd_filter(const struct bx_archive_buffer* input,
                                struct bx_archive_buffer* output,
                                bool decompress,
                                struct bx_diag_ctx* diag) {
#if BX_HAVE_LIBZSTD
    return decompress
        ? bx_archive_zstd_run_buffer_decode(input->data, input->len, output, diag)
        : bx_archive_zstd_run_buffer_encode(input->data, input->len, output, diag);
#else
    (void)input;
    (void)output;
    (void)decompress;
    bx_diag(diag, "zstd support is unavailable in this build");
    return false;
#endif
}

bool bx_archive_run_zstd_filter_stream(bx_archive_zstd_stream_producer_fn producer,
                                       void* producer_user,
                                       const struct bx_archive_zstd_stream_sink* output_sink,
                                       struct bx_diag_ctx* diag) {
#if BX_HAVE_LIBZSTD
    struct bx_archive_zstd_filter_stream_state state = {0};
    struct bx_archive_zstd_stream_sink input_sink;
    size_t rc;
    bool ok = false;

    if (producer == NULL || output_sink == NULL || output_sink->write == NULL) {
        bx_diag(diag, "invalid zstd stream configuration");
        return false;
    }

    state.output_sink = output_sink;
    state.diag = diag;
    state.stream = ZSTD_createCStream();
    if (state.stream == NULL) {
        bx_archive_zstd_diag_failed("compression", (size_t)-1, "stream allocation failed", diag);
        return false;
    }
    rc = ZSTD_initCStream(state.stream, ZSTD_CLEVEL_DEFAULT);
    if (ZSTD_isError(rc)) {
        bx_archive_zstd_diag_failed("compression", rc, NULL, diag);
        goto out;
    }
    state.outbuf_size = ZSTD_CStreamOutSize();
    state.outbuf = xmalloc(state.outbuf_size);

    input_sink.user = &state;
    input_sink.write = bx_archive_zstd_filter_stream_input_write;
    if (!producer(producer_user, &input_sink, diag)) {
        goto out;
    }
    ok = bx_archive_zstd_filter_stream_finish(&state);

out:
    free(state.outbuf);
    ZSTD_freeCStream(state.stream);
    return ok;
#else
    (void)producer;
    (void)producer_user;
    (void)output_sink;
    bx_diag(diag, "zstd support is unavailable in this build");
    return false;
#endif
}

bool bx_archive_zstd_reader_open(struct bx_archive_zstd_reader** reader_out,
                                 int fd,
                                 struct bx_diag_ctx* diag) {
#if BX_HAVE_LIBZSTD
    struct bx_archive_zstd_reader* reader;

    if (reader_out == NULL) {
        bx_diag(diag, "invalid zstd reader configuration");
        return false;
    }
    *reader_out = NULL;

    reader = xmalloc(sizeof(*reader));
    memset(reader, 0, sizeof(*reader));
    reader->fd = fd;
    reader->stream = ZSTD_createDStream();
    if (reader->stream == NULL) {
        free(reader);
        bx_archive_zstd_diag_failed("decompression", (size_t)-1, "stream allocation failed", diag);
        return false;
    }
    if (ZSTD_isError(ZSTD_initDStream(reader->stream))) {
        bx_archive_zstd_reader_close(reader);
        bx_archive_zstd_diag_failed("decompression", (size_t)-1, "decoder initialization failed", diag);
        return false;
    }
    reader->inbuf_size = ZSTD_DStreamInSize();
    reader->outbuf_size = ZSTD_DStreamOutSize();
    reader->inbuf = xmalloc(reader->inbuf_size);
    reader->outbuf = xmalloc(reader->outbuf_size);
    *reader_out = reader;
    return true;
#else
    (void)reader_out;
    (void)fd;
    bx_diag(diag, "zstd support is unavailable in this build");
    return false;
#endif
}

bool bx_archive_zstd_reader_read_some(struct bx_archive_zstd_reader* reader,
                                      unsigned char* buffer,
                                      size_t len,
                                      size_t* nread_out,
                                      struct bx_diag_ctx* diag) {
#if BX_HAVE_LIBZSTD
    size_t total = 0u;

    while (total < len) {
        size_t chunk;

        if (reader->out_pos == reader->out_len && !reader->finished) {
            if (!bx_archive_zstd_reader_fill_output(reader, diag)) {
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
        memcpy(buffer + total, (unsigned char*)reader->outbuf + reader->out_pos, chunk);
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
    bx_diag(diag, "zstd support is unavailable in this build");
    return false;
#endif
}

void bx_archive_zstd_reader_close(struct bx_archive_zstd_reader* reader) {
#if BX_HAVE_LIBZSTD
    if (reader == NULL) {
        return;
    }
    free(reader->inbuf);
    free(reader->outbuf);
    ZSTD_freeDStream(reader->stream);
    if (reader->fd >= 0) {
        close(reader->fd);
    }
    free(reader);
#else
    free(reader);
#endif
}
