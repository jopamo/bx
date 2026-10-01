#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ZLIB_CONST
#define ZLIB_CONST 1
#endif
#include <zlib.h>

#include "applets/archive/archive_bzip2.h"
#include "applets/archive/archive_codec.h"
#include "applets/archive/archive_common.h"
#include "applets/archive/archive_gzip.h"
#include "applets/archive/archive_xz.h"
#include "applets/archive/archive_zstd.h"
#include "applets/archive/archive_temp.h"
#include "bx/libbx.h"
#include "lib/fd_ops.h"

#define BX_ARCHIVE_GZIP_STREAM_BUFFER_SIZE (1024u * 1024u)

struct bx_archive_codec {
    const char* name;
    bool supports_mt_encode;
    bool (*matches_path_suffix)(const char* path);
};

struct bx_archive_codec_input {
    enum {
        BX_ARCHIVE_CODEC_INPUT_GZIP = 0,
        BX_ARCHIVE_CODEC_INPUT_BZIP2,
        BX_ARCHIVE_CODEC_INPUT_XZ,
        BX_ARCHIVE_CODEC_INPUT_ZSTD,
    } kind;
    gzFile stream;
    struct bx_fd_input source;
    unsigned char decode_buffer[8192];
    size_t decode_pos;
    size_t decode_len;
    bool source_eof;
    bool decoder_end;
    int feeder;
    void* codec_reader;
    const struct bx_archive_codec* required_codec;
    bool checked_mode;
    bool direct_mode;
    bool plain_seekable;
    bool force_seek;
    uint64_t plain_size;
    uint64_t logical_offset;
};

struct bx_archive_codec_producer_adapter {
    bx_archive_codec_stream_producer_fn producer;
    void* producer_user;
};

static bool bx_archive_codec_none_matches_path_suffix(const char* path) {
    (void)path;
    return false;
}

static const struct bx_archive_codec bx_archive_codec_none_value = {
    .name = "none",
    .supports_mt_encode = false,
    .matches_path_suffix = bx_archive_codec_none_matches_path_suffix,
};

static const struct bx_archive_codec bx_archive_codec_gzip_value = {
    .name = "gzip",
    .supports_mt_encode = true,
    .matches_path_suffix = bx_archive_path_has_gzip_suffix,
};

static bool bx_archive_codec_bzip2_matches_path_suffix(const char* path) {
    size_t len;

    if (path == NULL) {
        return false;
    }
    len = strlen(path);
    return (len >= 4u && strcmp(path + len - 4u, ".bz2") == 0)
        || (len >= 4u && strcmp(path + len - 4u, ".tbz") == 0)
        || (len >= 5u && strcmp(path + len - 5u, ".tbz2") == 0);
}

static const struct bx_archive_codec bx_archive_codec_bzip2_value = {
    .name = "bzip2",
    .supports_mt_encode = false,
    .matches_path_suffix = bx_archive_codec_bzip2_matches_path_suffix,
};

static bool bx_archive_codec_xz_matches_path_suffix(const char* path) {
    size_t len;

    if (path == NULL) {
        return false;
    }
    len = strlen(path);
    return (len >= 3u && strcmp(path + len - 3u, ".xz") == 0)
        || (len >= 4u && strcmp(path + len - 4u, ".txz") == 0);
}

static const struct bx_archive_codec bx_archive_codec_xz_value = {
    .name = "xz",
    .supports_mt_encode = false,
    .matches_path_suffix = bx_archive_codec_xz_matches_path_suffix,
};

static bool bx_archive_codec_zstd_matches_path_suffix(const char* path) {
    size_t len;

    if (path == NULL) {
        return false;
    }
    len = strlen(path);
    return (len >= 4u && strcmp(path + len - 4u, ".zst") == 0)
        || (len >= 5u && strcmp(path + len - 5u, ".tzst") == 0)
        || (len >= 5u && strcmp(path + len - 5u, ".zstd") == 0);
}

static const struct bx_archive_codec bx_archive_codec_zstd_value = {
    .name = "zstd",
    .supports_mt_encode = false,
    .matches_path_suffix = bx_archive_codec_zstd_matches_path_suffix,
};

const struct bx_archive_codec* bx_archive_codec_none(void) {
    return &bx_archive_codec_none_value;
}

const struct bx_archive_codec* bx_archive_codec_gzip(void) {
    return &bx_archive_codec_gzip_value;
}

const struct bx_archive_codec* bx_archive_codec_bzip2(void) {
    return &bx_archive_codec_bzip2_value;
}

const struct bx_archive_codec* bx_archive_codec_xz(void) {
    return &bx_archive_codec_xz_value;
}

const struct bx_archive_codec* bx_archive_codec_zstd(void) {
    return &bx_archive_codec_zstd_value;
}

const char* bx_archive_codec_name(const struct bx_archive_codec* codec) {
    return codec != NULL ? codec->name : bx_archive_codec_none_value.name;
}

bool bx_archive_codec_supports_mt_encode(const struct bx_archive_codec* codec) {
    return codec != NULL && codec->supports_mt_encode;
}

bool bx_archive_codec_matches_path_suffix(const struct bx_archive_codec* codec, const char* path) {
    return codec != NULL
        && codec->matches_path_suffix != NULL
        && path != NULL
        && codec->matches_path_suffix(path);
}

const struct bx_archive_codec* bx_archive_codec_detect_path_suffix(const char* path) {
    if (bx_archive_codec_matches_path_suffix(bx_archive_codec_gzip(), path)) {
        return bx_archive_codec_gzip();
    }
    if (bx_archive_codec_matches_path_suffix(bx_archive_codec_bzip2(), path)) {
        return bx_archive_codec_bzip2();
    }
    if (bx_archive_codec_matches_path_suffix(bx_archive_codec_xz(), path)) {
        return bx_archive_codec_xz();
    }
    if (bx_archive_codec_matches_path_suffix(bx_archive_codec_zstd(), path)) {
        return bx_archive_codec_zstd();
    }
    return NULL;
}

static bool bx_archive_codec_copy_buffer(const struct bx_archive_buffer* input,
                                         struct bx_archive_buffer* output,
                                         struct bx_diag_ctx* diag) {
    if (!bx_archive_buffer_append(output, input->data, input->len)) {
        bx_diag(diag, "buffer growth failed: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_archive_codec_gzip_producer_bridge(void* user,
                                                  const struct bx_archive_gzip_stream_sink* sink,
                                                  struct bx_diag_ctx* diag) {
    struct bx_archive_codec_producer_adapter* adapter = user;
    struct bx_archive_codec_stream_sink codec_sink = {
        .user = sink->user,
        .write = sink->write,
    };

    return adapter->producer(adapter->producer_user, &codec_sink, diag);
}

static bool bx_archive_codec_xz_producer_bridge(void* user,
                                                const struct bx_archive_xz_stream_sink* sink,
                                                struct bx_diag_ctx* diag) {
    struct bx_archive_codec_producer_adapter* adapter = user;
    struct bx_archive_codec_stream_sink codec_sink = {
        .user = sink->user,
        .write = sink->write,
    };

    return adapter->producer(adapter->producer_user, &codec_sink, diag);
}

static bool bx_archive_codec_bzip2_producer_bridge(void* user,
                                                   const struct bx_archive_bzip2_stream_sink* sink,
                                                   struct bx_diag_ctx* diag) {
    struct bx_archive_codec_producer_adapter* adapter = user;
    struct bx_archive_codec_stream_sink codec_sink = {
        .user = sink->user,
        .write = sink->write,
    };

    return adapter->producer(adapter->producer_user, &codec_sink, diag);
}

static bool bx_archive_codec_zstd_producer_bridge(void* user,
                                                  const struct bx_archive_zstd_stream_sink* sink,
                                                  struct bx_diag_ctx* diag) {
    struct bx_archive_codec_producer_adapter* adapter = user;
    struct bx_archive_codec_stream_sink codec_sink = {
        .user = sink->user,
        .write = sink->write,
    };

    return adapter->producer(adapter->producer_user, &codec_sink, diag);
}

bool bx_archive_codec_encode_buffer(const struct bx_archive_codec* codec,
                                    const struct bx_archive_buffer* input,
                                    struct bx_archive_buffer* output,
                                    struct bx_diag_ctx* diag) {
    if (codec == NULL || codec == bx_archive_codec_none()) {
        return bx_archive_codec_copy_buffer(input, output, diag);
    }
    if (codec == bx_archive_codec_gzip()) {
        return bx_archive_run_gzip_filter(input, output, false, diag);
    }
    if (codec == bx_archive_codec_bzip2()) {
        return bx_archive_run_bzip2_filter(input, output, false, diag);
    }
    if (codec == bx_archive_codec_xz()) {
        return bx_archive_run_xz_filter(input, output, false, diag);
    }
    if (codec == bx_archive_codec_zstd()) {
        return bx_archive_run_zstd_filter(input, output, false, diag);
    }
    bx_diag(diag, "unsupported archive codec '%s'", bx_archive_codec_name(codec));
    return false;
}

bool bx_archive_codec_decode_buffer(const struct bx_archive_codec* codec,
                                    const struct bx_archive_buffer* input,
                                    struct bx_archive_buffer* output,
                                    struct bx_diag_ctx* diag) {
    if (codec == NULL || codec == bx_archive_codec_none()) {
        return bx_archive_codec_copy_buffer(input, output, diag);
    }
    if (codec == bx_archive_codec_gzip()) {
        return bx_archive_run_gzip_filter(input, output, true, diag);
    }
    if (codec == bx_archive_codec_bzip2()) {
        return bx_archive_run_bzip2_filter(input, output, true, diag);
    }
    if (codec == bx_archive_codec_xz()) {
        return bx_archive_run_xz_filter(input, output, true, diag);
    }
    if (codec == bx_archive_codec_zstd()) {
        return bx_archive_run_zstd_filter(input, output, true, diag);
    }
    bx_diag(diag, "unsupported archive codec '%s'", bx_archive_codec_name(codec));
    return false;
}

bool bx_archive_codec_run_encode_stream(const struct bx_archive_codec* codec,
                                        bx_archive_codec_stream_producer_fn producer,
                                        void* producer_user,
                                        const struct bx_archive_codec_stream_sink* output_sink,
                                        struct bx_diag_ctx* diag) {
    if (producer == NULL || output_sink == NULL || output_sink->write == NULL) {
        bx_diag(diag, "invalid archive codec stream configuration");
        return false;
    }
    if (codec == NULL || codec == bx_archive_codec_none()) {
        return producer(producer_user, output_sink, diag);
    }
    if (codec == bx_archive_codec_gzip()) {
        struct bx_archive_codec_producer_adapter producer_adapter = {
            .producer = producer,
            .producer_user = producer_user,
        };
        struct bx_archive_gzip_stream_sink gzip_sink = {
            .user = output_sink->user,
            .write = output_sink->write,
        };

        return bx_archive_run_gzip_filter_stream(bx_archive_codec_gzip_producer_bridge,
                                                 &producer_adapter,
                                                 &gzip_sink,
                                                 diag);
    }
    if (codec == bx_archive_codec_bzip2()) {
        struct bx_archive_codec_producer_adapter producer_adapter = {
            .producer = producer,
            .producer_user = producer_user,
        };
        struct bx_archive_bzip2_stream_sink bzip2_sink = {
            .user = output_sink->user,
            .write = output_sink->write,
        };

        return bx_archive_run_bzip2_filter_stream(bx_archive_codec_bzip2_producer_bridge,
                                                  &producer_adapter,
                                                  &bzip2_sink,
                                                  diag);
    }
    if (codec == bx_archive_codec_xz()) {
        struct bx_archive_codec_producer_adapter producer_adapter = {
            .producer = producer,
            .producer_user = producer_user,
        };
        struct bx_archive_xz_stream_sink xz_sink = {
            .user = output_sink->user,
            .write = output_sink->write,
        };

        return bx_archive_run_xz_filter_stream(bx_archive_codec_xz_producer_bridge,
                                               &producer_adapter,
                                               &xz_sink,
                                               diag);
    }
    if (codec == bx_archive_codec_zstd()) {
        struct bx_archive_codec_producer_adapter producer_adapter = {
            .producer = producer,
            .producer_user = producer_user,
        };
        struct bx_archive_zstd_stream_sink zstd_sink = {
            .user = output_sink->user,
            .write = output_sink->write,
        };

        return bx_archive_run_zstd_filter_stream(bx_archive_codec_zstd_producer_bridge,
                                                 &producer_adapter,
                                                 &zstd_sink,
                                                 diag);
    }
    bx_diag(diag, "unsupported archive codec '%s'", bx_archive_codec_name(codec));
    return false;
}

bool bx_archive_codec_run_encode_mt_stream(const struct bx_archive_codec* codec,
                                           bx_archive_codec_stream_producer_fn producer,
                                           void* producer_user,
                                           const struct bx_archive_codec_stream_sink* output_sink,
                                           const struct bx_archive_codec_mt_options* mt_options,
                                           struct bx_diag_ctx* diag) {
    if (codec == NULL || codec == bx_archive_codec_none()) {
        bx_diag(diag, "archive codec '%s' does not support multithreaded encoding",
                bx_archive_codec_name(codec));
        return false;
    }
    if (mt_options == NULL) {
        bx_diag(diag, "invalid archive codec stream configuration");
        return false;
    }
    if (codec == bx_archive_codec_gzip()) {
        struct bx_archive_codec_producer_adapter producer_adapter = {
            .producer = producer,
            .producer_user = producer_user,
        };
        struct bx_archive_gzip_stream_sink gzip_sink = {
            .user = output_sink->user,
            .write = output_sink->write,
        };

        return bx_archive_run_gzip_filter_mt_stream(bx_archive_codec_gzip_producer_bridge,
                                                    &producer_adapter,
                                                    &gzip_sink,
                                                    mt_options->thread_count,
                                                    mt_options->chunk_size,
                                                    mt_options->max_inflight_chunks,
                                                    diag);
    }
    bx_diag(diag, "unsupported archive codec '%s'", bx_archive_codec_name(codec));
    return false;
}

static bool bx_archive_codec_input_requires_gzip(const struct bx_archive_codec_input* input) {
    return input->required_codec == bx_archive_codec_gzip();
}

static bool bx_archive_codec_input_requires_bzip2(const struct bx_archive_codec_input* input) {
    return input->required_codec == bx_archive_codec_bzip2();
}

static bool bx_archive_codec_input_requires_xz(const struct bx_archive_codec_input* input) {
    return input->required_codec == bx_archive_codec_xz();
}

static bool bx_archive_codec_input_requires_zstd(const struct bx_archive_codec_input* input) {
    return input->required_codec == bx_archive_codec_zstd();
}

static bool bx_archive_codec_input_feed(struct bx_archive_codec_input* input, struct bx_diag_ctx* diag) {
    unsigned char data[PIPE_BUF];
    ssize_t count = bx_fd_input_read(&input->source, data, sizeof(data), bx_archive_temp_signal_fd());
    if (count < 0) {
        bx_diag(diag, "read error: %s", strerror(errno));
        return false;
    }
    if (count == 0) {
        if (close(input->feeder) != 0) {
            input->feeder = -1;
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        input->feeder = -1;
        return true;
    }
    /* gzread reached EAGAIN, so this owned pipe is empty. PIPE_BUF fits
     * atomically without waiting, even when the pipe capacity is minimal. */
    ssize_t written = write(input->feeder, data, (size_t)count);
    if (written != count) {
        if (written >= 0)
            errno = EIO;
        bx_diag(diag, "read error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_archive_codec_input_check_mode(struct bx_archive_codec_input* input,
                                              struct bx_diag_ctx* diag) {
    int direct;

    if (input->kind != BX_ARCHIVE_CODEC_INPUT_GZIP) {
        return true;
    }
    if (input->checked_mode) {
        return true;
    }

    while (true) {
        direct = gzdirect(input->stream);
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        int error;
        (void)gzerror(input->stream, &error);
        if (input->source.fd < 0 || error != Z_ERRNO || (errno != EAGAIN && errno != EWOULDBLOCK))
            break;
        gzclearerr(input->stream);
        if (!bx_archive_codec_input_feed(input, diag))
            return false;
    }
    if (direct < 0) {
        bx_diag(diag, "gzip decompression failed");
        return false;
    }
    if (bx_archive_codec_input_requires_gzip(input) && direct == 1) {
        bx_diag(diag, "gzip decompression failed");
        return false;
    }
    input->direct_mode = direct == 1;
    input->checked_mode = true;
    return true;
}

static bool bx_archive_codec_bytes_have_magic(const unsigned char* data,
                                              size_t data_len,
                                              const unsigned char* magic,
                                              size_t magic_len) {
    return data_len >= magic_len && memcmp(data, magic, magic_len) == 0;
}

static const struct bx_archive_codec* bx_archive_codec_detect_magic(const unsigned char* data,
                                                                    size_t data_len) {
    static const unsigned char gzip_magic[] = {0x1f, 0x8b};
    static const unsigned char bzip2_magic[] = {'B', 'Z', 'h'};
    static const unsigned char xz_magic[] = {0xfd, '7', 'z', 'X', 'Z', 0x00};
    static const unsigned char zstd_magic[] = {0x28, 0xb5, 0x2f, 0xfd};

    if (bx_archive_codec_bytes_have_magic(data, data_len, gzip_magic, sizeof(gzip_magic))) {
        return bx_archive_codec_gzip();
    }
    if (bx_archive_codec_bytes_have_magic(data, data_len, bzip2_magic, sizeof(bzip2_magic))) {
        return bx_archive_codec_bzip2();
    }
    if (bx_archive_codec_bytes_have_magic(data, data_len, xz_magic, sizeof(xz_magic))) {
        return bx_archive_codec_xz();
    }
    if (bx_archive_codec_bytes_have_magic(data, data_len, zstd_magic, sizeof(zstd_magic))) {
        return bx_archive_codec_zstd();
    }
    return NULL;
}

const struct bx_archive_codec* bx_archive_codec_detect_fd(int fd) {
    unsigned char magic[6];
    ssize_t nread = pread(fd, magic, sizeof(magic), 0);

    if (nread <= 0) {
        return NULL;
    }
    return bx_archive_codec_detect_magic(magic, (size_t)nread);
}

static bool bx_archive_codec_input_open_source(struct bx_archive_codec_input** input_out,
                                               int fd,
                                               enum bx_fd_input_ownership ownership,
                                               const struct bx_archive_codec* required_codec,
                                               enum bx_archive_codec_seek_mode seek_mode,
                                               struct bx_diag_ctx* diag) {
    struct bx_archive_codec_input* input;

    if (input_out == NULL) {
        bx_diag(diag, "invalid archive codec reader configuration");
        return false;
    }

    *input_out = NULL;
    input = xmalloc(sizeof(*input));
    memset(input, 0, sizeof(*input));
    input->feeder = -1;
    input->source = (struct bx_fd_input)BX_FD_INPUT_INIT;
    input->force_seek = seek_mode == BX_ARCHIVE_CODEC_SEEK_FORCE;
    if (bx_fd_input_init(&input->source, fd, ownership) != 0) {
        int error = errno;
        if (ownership == BX_FD_INPUT_OWNED)
            close(fd);
        free(input);
        bx_diag(diag, "read error: %s", strerror(error));
        return false;
    }
    fd = input->source.fd;

    {
        struct stat st;

        if (fstat(fd, &st) == 0
            && S_ISREG(st.st_mode)
            && st.st_size >= 0) {
            input->plain_seekable = true;
            input->plain_size = (uint64_t)st.st_size;
        }
    }
    if (required_codec == NULL && input->plain_seekable) {
        required_codec = bx_archive_codec_detect_fd(fd);
    }
    if (seek_mode == BX_ARCHIVE_CODEC_SEEK_DISABLE) {
        input->plain_seekable = false;
    }
    input->required_codec = required_codec;

    if (bx_archive_codec_input_requires_bzip2(input)) {
        input->kind = BX_ARCHIVE_CODEC_INPUT_BZIP2;
        input->plain_seekable = false;
        input->plain_size = 0u;
        if (!bx_archive_bzip2_reader_open((struct bx_archive_bzip2_reader**)&input->codec_reader, diag)) {
            bx_archive_codec_input_close(input);
            return false;
        }
        *input_out = input;
        return true;
    }
    if (bx_archive_codec_input_requires_xz(input)) {
        input->kind = BX_ARCHIVE_CODEC_INPUT_XZ;
        input->plain_seekable = false;
        input->plain_size = 0u;
        if (!bx_archive_xz_reader_open((struct bx_archive_xz_reader**)&input->codec_reader, diag)) {
            bx_archive_codec_input_close(input);
            return false;
        }
        *input_out = input;
        return true;
    }
    if (bx_archive_codec_input_requires_zstd(input)) {
        input->kind = BX_ARCHIVE_CODEC_INPUT_ZSTD;
        input->plain_seekable = false;
        input->plain_size = 0u;
        if (!bx_archive_zstd_reader_open((struct bx_archive_zstd_reader**)&input->codec_reader, diag)) {
            bx_archive_codec_input_close(input);
            return false;
        }
        *input_out = input;
        return true;
    }

    input->kind = BX_ARCHIVE_CODEC_INPUT_GZIP;
    if (!S_ISREG(input->source.type)) {
        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) != 0) {
            bx_diag(diag, "read error: %s", strerror(errno));
            bx_archive_codec_input_close(input);
            return false;
        }
        fd = pipefd[0];
        input->feeder = pipefd[1];
    }
    input->stream = gzdopen(fd, "rb");
    if (input->stream == NULL) {
        if (input->feeder >= 0)
            close(fd);
        bx_archive_codec_input_close(input);
        bx_diag(diag, "failed to initialize archive reader");
        return false;
    }
    if (input->feeder < 0)
        input->source.fd = -1;
    if (bx_archive_codec_input_requires_gzip(input) && gzbuffer(input->stream, BX_ARCHIVE_GZIP_STREAM_BUFFER_SIZE) != 0) {
        bx_archive_codec_input_close(input);
        bx_diag(diag, "failed to initialize archive reader");
        return false;
    }

    *input_out = input;
    return true;
}

bool bx_archive_codec_input_open_fd(struct bx_archive_codec_input** input_out,
                                    int fd,
                                    const struct bx_archive_codec* required_codec,
                                    struct bx_diag_ctx* diag) {
    if (input_out == NULL || fd < 0) {
        bx_diag(diag, "invalid archive codec reader configuration");
        return false;
    }
    return bx_archive_codec_input_open_source(input_out, fd, BX_FD_INPUT_DUPLICATE_BORROWED, required_codec, BX_ARCHIVE_CODEC_SEEK_AUTO, diag);
}

bool bx_archive_codec_input_open(struct bx_archive_codec_input** input_out,
                                 const struct bx_archive_codec_input_options* options,
                                 struct bx_diag_ctx* diag) {
    int fd;

    if (input_out == NULL || options == NULL) {
        bx_diag(diag, "invalid archive codec reader configuration");
        return false;
    }

    if (options->archive_path == NULL || strcmp(options->archive_path, "-") == 0) {
        return bx_archive_codec_input_open_source(input_out, STDIN_FILENO, BX_FD_INPUT_DUPLICATE_BORROWED, options->required_codec, options->seek_mode, diag);
    }
    else {
        fd = bx_fd_open_cloexec(options->archive_path, O_RDONLY, 0);
        if (fd < 0) {
            bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
            return false;
        }
    }

    return bx_archive_codec_input_open_source(input_out, fd, BX_FD_INPUT_OWNED, options->required_codec, options->seek_mode, diag);
}

static bool bx_archive_codec_input_read_decoded(struct bx_archive_codec_input* input, unsigned char* buffer, size_t len, size_t* nread_out, struct bx_diag_ctx* diag) {
    *nread_out = 0u;
    while (!input->decoder_end) {
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        struct bx_archive_decode_chunk chunk = {
            .input = input->decode_buffer + input->decode_pos,
            .input_size = input->decode_len - input->decode_pos,
            .output = buffer,
            .output_size = len > sizeof(input->decode_buffer) ? sizeof(input->decode_buffer) : len,
            .input_eof = input->source_eof,
        };
        enum bx_archive_decode_result result;
        if (input->kind == BX_ARCHIVE_CODEC_INPUT_BZIP2)
            result = bx_archive_bzip2_reader_decode(input->codec_reader, &chunk);
        else if (input->kind == BX_ARCHIVE_CODEC_INPUT_XZ)
            result = bx_archive_xz_reader_decode(input->codec_reader, &chunk);
        else
            result = bx_archive_zstd_reader_decode(input->codec_reader, &chunk);
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        if (result == BX_ARCHIVE_DECODE_ERROR) {
            bx_diag(diag, "%s decompression failed%s%s", bx_archive_codec_name(input->required_codec), chunk.error_detail ? ": " : "", chunk.error_detail ? chunk.error_detail : "");
            return false;
        }
        input->decode_pos += chunk.input_used;
        input->decoder_end = result == BX_ARCHIVE_DECODE_END;
        if (chunk.output_used) {
            *nread_out = chunk.output_used;
            input->logical_offset += chunk.output_used;
            return true;
        }
        if (input->decoder_end)
            return true;
        if (chunk.input_used)
            continue;
        size_t remaining = input->decode_len - input->decode_pos;
        if (input->source_eof || remaining == sizeof(input->decode_buffer)) {
            bx_diag(diag, "%s decompression failed: decoder made no progress", bx_archive_codec_name(input->required_codec));
            return false;
        }
        memmove(input->decode_buffer, input->decode_buffer + input->decode_pos, remaining);
        input->decode_pos = 0u;
        input->decode_len = remaining;
        ssize_t count = bx_fd_input_read(&input->source, input->decode_buffer + remaining, sizeof(input->decode_buffer) - remaining, bx_archive_temp_signal_fd());
        if (count < 0) {
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        input->source_eof = count == 0;
        input->decode_len += (size_t)count;
    }
    return true;
}

bool bx_archive_codec_input_read_some(struct bx_archive_codec_input* input,
                                      unsigned char* buffer,
                                      size_t len,
                                      size_t* nread_out,
                                      struct bx_diag_ctx* diag) {
    if (len == 0) {
        *nread_out = 0;
        return true;
    }
    if (bx_archive_temp_pending_signal()) {
        errno = EINTR;
        bx_diag(diag, "read error: %s", strerror(errno));
        return false;
    }
    if (input->kind != BX_ARCHIVE_CODEC_INPUT_GZIP)
        return bx_archive_codec_input_read_decoded(input, buffer, len, nread_out, diag);

    unsigned int request = len > INT_MAX ? INT_MAX : (unsigned int)len;
    int nread;

    while (true) {
        nread = gzread(input->stream, buffer, request);
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        int error;
        (void)gzerror(input->stream, &error);
        if (input->source.fd < 0 || error != Z_ERRNO || (errno != EAGAIN && errno != EWOULDBLOCK))
            break;
        gzclearerr(input->stream);
        if (nread > 0)
            break;
        if (!bx_archive_codec_input_feed(input, diag))
            return false;
    }
    if (nread < 0) {
        int errnum = 0;
        const char* msg = gzerror(input->stream, &errnum);

        if (errnum == Z_ERRNO) {
            bx_diag(diag, "read error: %s", strerror(errno));
        }
        else {
            bx_diag(diag,
                    "gzip decompression failed%s%s",
                    msg != NULL ? ": " : "",
                    msg != NULL ? msg : "");
        }
        return false;
    }

    if (!bx_archive_codec_input_check_mode(input, diag)) {
        return false;
    }

    *nread_out = (size_t)nread;
    input->logical_offset += (uint64_t)(size_t)nread;
    return true;
}

static bool bx_archive_codec_input_skip_direct_seek(struct bx_archive_codec_input* input,
                                                    size_t len,
                                                    struct bx_diag_ctx* diag) {
    while (len > 0u) {
        size_t chunk = len > (size_t)LONG_MAX ? (size_t)LONG_MAX : len;

        if (input->logical_offset > input->plain_size
            || chunk > input->plain_size - input->logical_offset) {
            bx_diag(diag, "truncated archive");
            return false;
        }
        if (gzseek(input->stream, (z_off_t)chunk, SEEK_CUR) < 0) {
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        input->logical_offset += chunk;
        len -= chunk;
    }

    return true;
}

static bool bx_archive_codec_input_try_seek_direct(struct bx_archive_codec_input* input,
                                                    size_t len,
                                                    size_t* remaining_out) {
    if (remaining_out == NULL) {
        return false;
    }

    *remaining_out = len;
    while (len > 0u) {
        size_t chunk = len > (size_t)LONG_MAX ? (size_t)LONG_MAX : len;

        if (input->kind == BX_ARCHIVE_CODEC_INPUT_GZIP) {
            if (gzseek(input->stream, (z_off_t)chunk, SEEK_CUR) < 0) {
                return false;
            }
        }
        else {
            return false;
        }
        input->logical_offset += chunk;
        len -= chunk;
        *remaining_out = len;
    }

    return true;
}

bool bx_archive_codec_input_skip(struct bx_archive_codec_input* input,
                                 size_t len,
                                 struct bx_diag_ctx* diag) {
    unsigned char buffer[8192];

    if (len == 0u) {
        return true;
    }
    if (input->kind == BX_ARCHIVE_CODEC_INPUT_GZIP
        && !bx_archive_codec_input_check_mode(input, diag)) {
        return false;
    }
    if (input->force_seek && !input->plain_seekable && input->kind == BX_ARCHIVE_CODEC_INPUT_GZIP && input->direct_mode) {
        size_t remaining = len;

        if (bx_archive_codec_input_try_seek_direct(input, len, &remaining)) {
            return true;
        }
        input->force_seek = false;
        len = remaining;
    }
    if (input->kind == BX_ARCHIVE_CODEC_INPUT_GZIP
        && input->direct_mode
        && input->plain_seekable) {
        return bx_archive_codec_input_skip_direct_seek(input, len, diag);
    }

    while (len > 0u) {
        size_t nread = 0u;

        if (!bx_archive_codec_input_read_some(input,
                                             buffer,
                                             len > sizeof(buffer) ? sizeof(buffer) : len,
                                             &nread,
                                             diag)) {
            return false;
        }
        if (nread == 0u) {
            bx_diag(diag, "truncated archive");
            return false;
        }
        len -= nread;
    }

    return true;
}

static bool bx_archive_codec_input_drain_to_eof(struct bx_archive_codec_input* input,
                                                struct bx_diag_ctx* diag) {
    unsigned char buffer[8192];

    while (true) {
        size_t nread = 0u;

        if (!bx_archive_codec_input_read_some(input, buffer, sizeof(buffer), &nread, diag)) {
            return false;
        }
        if (nread == 0u) {
            return true;
        }
    }
}

bool bx_archive_codec_input_finish_success(struct bx_archive_codec_input* input,
                                           struct bx_diag_ctx* diag) {
    if (input->kind == BX_ARCHIVE_CODEC_INPUT_GZIP
        && !bx_archive_codec_input_check_mode(input, diag)) {
        return false;
    }
    if (input->kind == BX_ARCHIVE_CODEC_INPUT_GZIP
        && input->direct_mode
        && input->plain_seekable) {
        return true;
    }
    return bx_archive_codec_input_drain_to_eof(input, diag);
}

uint64_t bx_archive_codec_input_total_bytes_read(const struct bx_archive_codec_input* input) {
    return input != NULL ? input->logical_offset : 0u;
}

void bx_archive_codec_input_close(struct bx_archive_codec_input* input) {
    if (input == NULL) {
        return;
    }
    if (input->kind == BX_ARCHIVE_CODEC_INPUT_XZ) {
        bx_archive_xz_reader_close((struct bx_archive_xz_reader*)input->codec_reader);
    }
    else if (input->kind == BX_ARCHIVE_CODEC_INPUT_BZIP2) {
        bx_archive_bzip2_reader_close((struct bx_archive_bzip2_reader*)input->codec_reader);
    }
    else if (input->kind == BX_ARCHIVE_CODEC_INPUT_ZSTD) {
        bx_archive_zstd_reader_close((struct bx_archive_zstd_reader*)input->codec_reader);
    }
    else if (input->stream != NULL) {
        gzclose(input->stream);
    }
    bx_fd_input_close(&input->source);
    bx_fd_cleanup(&input->feeder);
    free(input);
}
