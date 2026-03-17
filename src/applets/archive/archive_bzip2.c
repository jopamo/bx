#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets/archive/archive_bzip2.h"
#include "bx/libbx.h"

#if BX_HAVE_LIBBZ2
#include <bzlib.h>
#endif

#define BX_ARCHIVE_BZIP2_IO_CHUNK 8192u
#define BX_ARCHIVE_BZIP2_READER_IN_CHUNK 8192u
#define BX_ARCHIVE_BZIP2_READER_OUT_CHUNK 8192u

struct bx_archive_bzip2_reader {
#if BX_HAVE_LIBBZ2
    int fd;
    bz_stream stream;
    unsigned char inbuf[BX_ARCHIVE_BZIP2_READER_IN_CHUNK];
    unsigned char outbuf[BX_ARCHIVE_BZIP2_READER_OUT_CHUNK];
    size_t out_pos;
    size_t out_len;
    bool input_eof;
    bool finished;
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

static bool bx_archive_bzip2_reader_reinit_stream(struct bx_archive_bzip2_reader* reader,
                                                  struct bx_diag_ctx* diag) {
    char* next_in = reader->stream.next_in;
    unsigned int avail_in = reader->stream.avail_in;
    int rc;

    BZ2_bzDecompressEnd(&reader->stream);
    memset(&reader->stream, 0, sizeof(reader->stream));
    rc = BZ2_bzDecompressInit(&reader->stream, 0, 0);
    if (rc != BZ_OK) {
        reader->stream_initialized = false;
        bx_archive_bzip2_diag_failed("decompression", rc, diag);
        return false;
    }
    reader->stream.next_in = next_in;
    reader->stream.avail_in = avail_in;
    reader->stream_initialized = true;
    return true;
}

static bool bx_archive_bzip2_reader_fill_input(struct bx_archive_bzip2_reader* reader,
                                               struct bx_diag_ctx* diag) {
    ssize_t nread = read(reader->fd, reader->inbuf, sizeof(reader->inbuf));

    if (nread < 0) {
        bx_diag(diag, "read error: %s", strerror(errno));
        return false;
    }
    if (nread == 0) {
        reader->input_eof = true;
    }
    reader->stream.next_in = (char*)reader->inbuf;
    reader->stream.avail_in = nread > 0 ? (unsigned int)nread : 0u;
    return true;
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

static bool bx_archive_bzip2_reader_fill_output(struct bx_archive_bzip2_reader* reader,
                                                struct bx_diag_ctx* diag) {
    while (reader->out_pos == reader->out_len && !reader->finished) {
        int rc;
        size_t produced;

        if (reader->stream.avail_in == 0u && !reader->input_eof) {
            if (!bx_archive_bzip2_reader_fill_input(reader, diag)) {
                return false;
            }
        }

        if (reader->stream.avail_in == 0u && reader->input_eof) {
            bx_archive_bzip2_diag_failed("decompression", BZ_UNEXPECTED_EOF, diag);
            return false;
        }

        reader->stream.next_out = (char*)reader->outbuf;
        reader->stream.avail_out = sizeof(reader->outbuf);
        rc = BZ2_bzDecompress(&reader->stream);
        produced = sizeof(reader->outbuf) - (size_t)reader->stream.avail_out;

        if (rc == BZ_STREAM_END) {
            bool need_restart = reader->stream.avail_in > 0u;

            if (need_restart) {
                if (!bx_archive_bzip2_reader_reinit_stream(reader, diag)) {
                    return false;
                }
            }
            else if (reader->input_eof) {
                reader->finished = true;
            }
            else {
                if (!bx_archive_bzip2_reader_fill_input(reader, diag)) {
                    return false;
                }
                if (reader->stream.avail_in == 0u && reader->input_eof) {
                    reader->finished = true;
                }
                else if (!bx_archive_bzip2_reader_reinit_stream(reader, diag)) {
                    return false;
                }
            }
            if (produced > 0u) {
                reader->out_pos = 0u;
                reader->out_len = produced;
                return true;
            }
            continue;
        }
        if (rc != BZ_OK) {
            bx_archive_bzip2_diag_failed("decompression", rc, diag);
            return false;
        }
        if (produced > 0u) {
            reader->out_pos = 0u;
            reader->out_len = produced;
            return true;
        }
    }

    return true;
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
                                        struct bx_diag_ctx* diag) {
    struct bx_archive_bzip2_filter_stream_state state = {0};
    struct bx_archive_bzip2_stream_sink input_sink;
    bool ok = false;
    int rc;

    if (producer == NULL || output_sink == NULL || output_sink->write == NULL) {
        bx_diag(diag, "invalid bzip2 stream configuration");
        return false;
    }

    rc = BZ2_bzCompressInit(&state.stream, 9, 0, 30);
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

bool bx_archive_bzip2_reader_open(struct bx_archive_bzip2_reader** reader_out,
                                  int fd,
                                  struct bx_diag_ctx* diag) {
    struct bx_archive_bzip2_reader* reader;
    int rc;

    if (reader_out == NULL || fd < 0) {
        bx_diag(diag, "invalid bzip2 reader configuration");
        return false;
    }

    *reader_out = NULL;
    reader = xmalloc(sizeof(*reader));
    memset(reader, 0, sizeof(*reader));
    reader->fd = fd;

    rc = BZ2_bzDecompressInit(&reader->stream, 0, 0);
    if (rc != BZ_OK) {
        free(reader);
        bx_archive_bzip2_diag_failed("decompression", rc, diag);
        return false;
    }
    reader->stream_initialized = true;

    *reader_out = reader;
    return true;
}

bool bx_archive_bzip2_reader_read_some(struct bx_archive_bzip2_reader* reader,
                                       unsigned char* buffer,
                                       size_t len,
                                       size_t* nread_out,
                                       struct bx_diag_ctx* diag) {
    size_t available;

    if (reader == NULL || buffer == NULL || nread_out == NULL) {
        bx_diag(diag, "invalid bzip2 reader configuration");
        return false;
    }

    *nread_out = 0u;
    if (len == 0u) {
        return true;
    }

    if (reader->out_pos == reader->out_len) {
        reader->out_pos = 0u;
        reader->out_len = 0u;
        if (!bx_archive_bzip2_reader_fill_output(reader, diag)) {
            return false;
        }
    }

    available = reader->out_len - reader->out_pos;
    if (available == 0u) {
        return true;
    }
    if (available > len) {
        available = len;
    }
    memcpy(buffer, reader->outbuf + reader->out_pos, available);
    reader->out_pos += available;
    *nread_out = available;
    return true;
}

void bx_archive_bzip2_reader_close(struct bx_archive_bzip2_reader* reader) {
    if (reader == NULL) {
        return;
    }
    if (reader->stream_initialized) {
        BZ2_bzDecompressEnd(&reader->stream);
    }
    if (reader->fd >= 0) {
        close(reader->fd);
    }
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
                                        struct bx_diag_ctx* diag) {
    (void)producer;
    (void)producer_user;
    (void)output_sink;
    bx_diag(diag, "bzip2 support is unavailable in this build");
    return false;
}

bool bx_archive_bzip2_reader_open(struct bx_archive_bzip2_reader** reader_out,
                                  int fd,
                                  struct bx_diag_ctx* diag) {
    (void)reader_out;
    (void)fd;
    bx_diag(diag, "bzip2 support is unavailable in this build");
    return false;
}

bool bx_archive_bzip2_reader_read_some(struct bx_archive_bzip2_reader* reader,
                                       unsigned char* buffer,
                                       size_t len,
                                       size_t* nread_out,
                                       struct bx_diag_ctx* diag) {
    (void)reader;
    (void)buffer;
    (void)len;
    (void)nread_out;
    bx_diag(diag, "bzip2 support is unavailable in this build");
    return false;
}

void bx_archive_bzip2_reader_close(struct bx_archive_bzip2_reader* reader) {
    (void)reader;
}
#endif
