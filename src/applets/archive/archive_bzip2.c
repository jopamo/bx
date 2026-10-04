#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/archive/archive_bzip2.h"
#include "bx/libbx.h"

#if BX_HAVE_LIBBZ2
#include <bzlib.h>
#endif

#define BX_ARCHIVE_BZIP2_IO_CHUNK 8192u

struct bx_archive_bzip2_reader {
#if BX_HAVE_LIBBZ2
    bz_stream stream;
    bool at_end;
    bool stream_initialized;
#else
    int unused;
#endif
};

#if BX_HAVE_LIBBZ2
static const char* bx_archive_bzip2_ret_detail(int rc) {
    switch (rc) {
        case BZ_OK:
        case BZ_RUN_OK:
        case BZ_FLUSH_OK:
        case BZ_FINISH_OK:
        case BZ_STREAM_END:
            return NULL;
        case BZ_SEQUENCE_ERROR:
            return "internal codec error";
        case BZ_PARAM_ERROR:
            return "invalid codec parameters";
        case BZ_MEM_ERROR:
            return "memory allocation failed";
        case BZ_DATA_ERROR:
            return "compressed data is corrupt";
        case BZ_DATA_ERROR_MAGIC:
            return "file format not recognized";
        case BZ_IO_ERROR:
            return "i/o error";
        case BZ_UNEXPECTED_EOF:
            return "compressed data is truncated";
        case BZ_OUTBUFF_FULL:
            return "output buffer is too small";
        case BZ_CONFIG_ERROR:
            return "library configuration error";
        default:
            return "codec error";
    }
}

static void bx_archive_bzip2_diag_failed(const char* action, int rc, struct bx_diag_ctx* diag) {
    const char* detail = bx_archive_bzip2_ret_detail(rc);

    bx_diag(diag,
            "bzip2 %s failed%s%s",
            action,
            detail != NULL ? ": " : "",
            detail != NULL ? detail : "");
}

static bool bx_archive_bzip2_write_output(const struct bx_archive_bzip2_stream_sink* sink,
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

static int bx_archive_bzip2_reader_reinit_stream(struct bx_archive_bzip2_reader* reader) {
    BZ2_bzDecompressEnd(&reader->stream);
    memset(&reader->stream, 0, sizeof(reader->stream));
    int rc = BZ2_bzDecompressInit(&reader->stream, 0, 0);
    reader->stream_initialized = rc == BZ_OK;
    return rc;
}

static bool bx_archive_bzip2_run_buffer_filter(const unsigned char* input,
                                               size_t input_len,
                                               struct bx_archive_buffer* output,
                                               bool decompress,
                                               struct bx_diag_ctx* diag) {
    bz_stream stream;
    size_t input_pos = 0u;
    bool stream_initialized = false;
    bool ok = false;
    int rc;

    memset(&stream, 0, sizeof(stream));
    rc = decompress
        ? BZ2_bzDecompressInit(&stream, 0, 0)
        : BZ2_bzCompressInit(&stream, 9, 0, 30);
    if (rc != BZ_OK) {
        bx_archive_bzip2_diag_failed(decompress ? "decompression" : "compression", rc, diag);
        return false;
    }
    stream_initialized = true;

    for (;;) {
        unsigned char outbuf[BX_ARCHIVE_BZIP2_IO_CHUNK];
        size_t produced;

        if (stream.avail_in == 0u && input_pos < input_len) {
            size_t chunk = input_len - input_pos;

            if (chunk > UINT_MAX) {
                chunk = UINT_MAX;
            }
            stream.next_in = (char*)(uintptr_t)(input + input_pos);
            stream.avail_in = (unsigned int)chunk;
            input_pos += chunk;
        }

        if (decompress && stream.avail_in == 0u && input_pos == input_len) {
            bx_archive_bzip2_diag_failed("decompression", BZ_UNEXPECTED_EOF, diag);
            break;
        }

        stream.next_out = (char*)outbuf;
        stream.avail_out = sizeof(outbuf);
        rc = decompress
            ? BZ2_bzDecompress(&stream)
            : BZ2_bzCompress(&stream,
                             (stream.avail_in == 0u && input_pos == input_len) ? BZ_FINISH : BZ_RUN);
        produced = sizeof(outbuf) - (size_t)stream.avail_out;
        if (produced > 0u && !bx_archive_buffer_append(output, outbuf, produced)) {
            if (decompress && stream_initialized) {
                BZ2_bzDecompressEnd(&stream);
            }
            else if (!decompress) {
                BZ2_bzCompressEnd(&stream);
            }
            bx_diag(diag, "buffer growth failed: %s", strerror(errno));
            return false;
        }

        if (decompress) {
            if (rc == BZ_STREAM_END) {
                if (stream.avail_in == 0u && input_pos == input_len) {
                    ok = true;
                    break;
                }

                {
                    char* next_in = stream.next_in;
                    unsigned int avail_in = stream.avail_in;

                    BZ2_bzDecompressEnd(&stream);
                    stream_initialized = false;
                    memset(&stream, 0, sizeof(stream));
                    rc = BZ2_bzDecompressInit(&stream, 0, 0);
                    if (rc != BZ_OK) {
                        bx_archive_bzip2_diag_failed("decompression", rc, diag);
                        goto out;
                    }
                    stream_initialized = true;
                    stream.next_in = next_in;
                    stream.avail_in = avail_in;
                }
                continue;
            }
            if (rc != BZ_OK) {
                bx_archive_bzip2_diag_failed("decompression", rc, diag);
                break;
            }
        }
        else {
            if (rc == BZ_STREAM_END) {
                ok = true;
                break;
            }
            if (rc != BZ_RUN_OK && rc != BZ_FINISH_OK) {
                bx_archive_bzip2_diag_failed("compression", rc, diag);
                break;
            }
        }
    }

out:
    if (decompress && stream_initialized) {
        BZ2_bzDecompressEnd(&stream);
    }
    else {
        BZ2_bzCompressEnd(&stream);
    }
    return ok;
}

struct bx_archive_bzip2_filter_stream_state {
    const struct bx_archive_bzip2_stream_sink* output_sink;
    struct bx_diag_ctx* diag;
    bz_stream stream;
    bool stream_initialized;
};

static bool bx_archive_bzip2_filter_stream_feed(struct bx_archive_bzip2_filter_stream_state* state,
                                                const unsigned char* data,
                                                size_t len) {
    while (len > 0u) {
        unsigned char outbuf[BX_ARCHIVE_BZIP2_IO_CHUNK];
        size_t chunk = len;
        size_t produced;
        int rc;

        if (chunk > UINT_MAX) {
            chunk = UINT_MAX;
        }
        state->stream.next_in = (char*)(uintptr_t)data;
        state->stream.avail_in = (unsigned int)chunk;
        data += chunk;
        len -= chunk;

        while (state->stream.avail_in > 0u) {
            state->stream.next_out = (char*)outbuf;
            state->stream.avail_out = sizeof(outbuf);
            rc = BZ2_bzCompress(&state->stream, BZ_RUN);
            if (rc != BZ_RUN_OK) {
                bx_archive_bzip2_diag_failed("compression", rc, state->diag);
                return false;
            }
            produced = sizeof(outbuf) - (size_t)state->stream.avail_out;
            if (!bx_archive_bzip2_write_output(state->output_sink, outbuf, produced, state->diag)) {
                return false;
            }
        }
    }

    return true;
}

static bool bx_archive_bzip2_filter_stream_finish(struct bx_archive_bzip2_filter_stream_state* state) {
    for (;;) {
        unsigned char outbuf[BX_ARCHIVE_BZIP2_IO_CHUNK];
        size_t produced;
        int rc;

        state->stream.next_out = (char*)outbuf;
        state->stream.avail_out = sizeof(outbuf);
        rc = BZ2_bzCompress(&state->stream, BZ_FINISH);
        produced = sizeof(outbuf) - (size_t)state->stream.avail_out;
        if (!bx_archive_bzip2_write_output(state->output_sink, outbuf, produced, state->diag)) {
            return false;
        }
        if (rc == BZ_STREAM_END) {
            return true;
        }
        if (rc != BZ_FINISH_OK) {
            bx_archive_bzip2_diag_failed("compression", rc, state->diag);
            return false;
        }
    }
}

static bool bx_archive_bzip2_filter_stream_input_write(void* user, const void* data, size_t len) {
    struct bx_archive_bzip2_filter_stream_state* state = user;
    return bx_archive_bzip2_filter_stream_feed(state, data, len);
}

bool bx_archive_run_bzip2_filter(const struct bx_archive_buffer* input,
                                 struct bx_archive_buffer* output,
                                 bool decompress,
                                 struct bx_diag_ctx* diag) {
    if (input == NULL || output == NULL) {
        bx_diag(diag, "invalid bzip2 buffer configuration");
        return false;
    }
    return bx_archive_bzip2_run_buffer_filter(input->data, input->len, output, decompress, diag);
}

bool bx_archive_run_bzip2_filter_stream(bx_archive_bzip2_stream_producer_fn producer,
                                        void* producer_user,
                                        const struct bx_archive_bzip2_stream_sink* output_sink,
                                        int level,
                                        struct bx_diag_ctx* diag) {
    struct bx_archive_bzip2_filter_stream_state state = {0};
    struct bx_archive_bzip2_stream_sink input_sink;
    bool ok = false;
    int rc;

    if (producer == NULL || output_sink == NULL || output_sink->write == NULL) {
        bx_diag(diag, "invalid bzip2 stream configuration");
        return false;
    }

    rc = BZ2_bzCompressInit(&state.stream, level < 0 ? 9 : level, 0, 30);
    if (rc != BZ_OK) {
        bx_archive_bzip2_diag_failed("compression", rc, diag);
        return false;
    }
    state.stream_initialized = true;
    state.output_sink = output_sink;
    state.diag = diag;

    input_sink.user = &state;
    input_sink.write = bx_archive_bzip2_filter_stream_input_write;

    if (producer(producer_user, &input_sink, diag)) {
        ok = bx_archive_bzip2_filter_stream_finish(&state);
    }

    BZ2_bzCompressEnd(&state.stream);
    return ok;
}

bool bx_archive_bzip2_reader_open(struct bx_archive_bzip2_reader** reader_out, struct bx_diag_ctx* diag) {
    if (!reader_out) {
        bx_diag(diag, "invalid bzip2 reader configuration");
        return false;
    }
    *reader_out = NULL;
    struct bx_archive_bzip2_reader* reader = xmalloc(sizeof(*reader));
    memset(reader, 0, sizeof(*reader));
    int rc = BZ2_bzDecompressInit(&reader->stream, 0, 0);
    if (rc != BZ_OK) {
        free(reader);
        bx_archive_bzip2_diag_failed("decompression", rc, diag);
        return false;
    }
    reader->stream_initialized = true;
    *reader_out = reader;
    return true;
}

enum bx_archive_decode_result bx_archive_bzip2_reader_decode(struct bx_archive_bzip2_reader* reader, struct bx_archive_decode_chunk* chunk) {
    chunk->input_used = chunk->output_used = 0u;
    chunk->error_detail = NULL;
    if (chunk->input_size > UINT_MAX || chunk->output_size > UINT_MAX) {
        chunk->error_detail = "invalid codec parameters";
        return BX_ARCHIVE_DECODE_ERROR;
    }
    if (reader->at_end) {
        if (!chunk->input_size)
            return chunk->input_eof ? BX_ARCHIVE_DECODE_END : BX_ARCHIVE_DECODE_MORE;
        int rc = bx_archive_bzip2_reader_reinit_stream(reader);
        if (rc != BZ_OK) {
            chunk->error_detail = bx_archive_bzip2_ret_detail(rc);
            return BX_ARCHIVE_DECODE_ERROR;
        }
        reader->at_end = false;
    }
    reader->stream.next_in = (char*)(uintptr_t)chunk->input;
    reader->stream.avail_in = (unsigned int)chunk->input_size;
    reader->stream.next_out = (char*)chunk->output;
    reader->stream.avail_out = (unsigned int)chunk->output_size;
    int rc = BZ2_bzDecompress(&reader->stream);
    chunk->input_used = chunk->input_size - reader->stream.avail_in;
    chunk->output_used = chunk->output_size - reader->stream.avail_out;
    reader->stream.next_in = reader->stream.next_out = NULL;
    reader->stream.avail_in = reader->stream.avail_out = 0u;
    if (rc == BZ_STREAM_END) {
        reader->at_end = true;
        return chunk->input_eof && chunk->input_used == chunk->input_size ? BX_ARCHIVE_DECODE_END : BX_ARCHIVE_DECODE_MORE;
    }
    if (rc == BZ_OK && chunk->input_eof && !chunk->input_used && !chunk->output_used)
        rc = BZ_UNEXPECTED_EOF;
    if (rc != BZ_OK) {
        chunk->error_detail = bx_archive_bzip2_ret_detail(rc);
        return BX_ARCHIVE_DECODE_ERROR;
    }
    return BX_ARCHIVE_DECODE_MORE;
}

void bx_archive_bzip2_reader_close(struct bx_archive_bzip2_reader* reader) {
    if (!reader)
        return;
    if (reader->stream_initialized)
        BZ2_bzDecompressEnd(&reader->stream);
    free(reader);
}

#else
bool bx_archive_run_bzip2_filter(const struct bx_archive_buffer* input,
                                 struct bx_archive_buffer* output,
                                 bool decompress,
                                 struct bx_diag_ctx* diag) {
    (void)input;
    (void)output;
    (void)decompress;
    bx_diag(diag, "bzip2 support is unavailable in this build");
    return false;
}

bool bx_archive_run_bzip2_filter_stream(bx_archive_bzip2_stream_producer_fn producer,
                                        void* producer_user,
                                        const struct bx_archive_bzip2_stream_sink* output_sink,
                                        int level,
                                        struct bx_diag_ctx* diag) {
    (void)producer;
    (void)producer_user;
    (void)output_sink;
    (void)level;
    bx_diag(diag, "bzip2 support is unavailable in this build");
    return false;
}

bool bx_archive_bzip2_reader_open(struct bx_archive_bzip2_reader** reader_out, struct bx_diag_ctx* diag) {
    (void)reader_out;
    bx_diag(diag, "bzip2 support is unavailable in this build");
    return false;
}

enum bx_archive_decode_result bx_archive_bzip2_reader_decode(struct bx_archive_bzip2_reader* reader, struct bx_archive_decode_chunk* chunk) {
    (void)reader;
    chunk->error_detail = "support is unavailable in this build";
    return BX_ARCHIVE_DECODE_ERROR;
}

void bx_archive_bzip2_reader_close(struct bx_archive_bzip2_reader* reader) {
    (void)reader;
}
#endif
