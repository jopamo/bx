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

#include "applets/archive/archive_bzip2.h"
#include "applets/archive/archive_codec.h"
#include "applets/archive/archive_common.h"
#include "applets/archive/archive_gzip.h"
#include "applets/archive/archive_xz.h"
#include "applets/archive/archive_zstd.h"
#include "applets/archive/archive_temp.h"
#include "bx/libbx.h"
#include "lib/fd_ops.h"

struct bx_archive_codec {
    const char* name;
    bool supports_mt_encode;
    bool (*matches_path_suffix)(const char* path);
};

struct bx_archive_codec_input {
    enum {
        BX_ARCHIVE_CODEC_INPUT_PLAIN = 0,
        BX_ARCHIVE_CODEC_INPUT_GZIP,
        BX_ARCHIVE_CODEC_INPUT_BZIP2,
        BX_ARCHIVE_CODEC_INPUT_XZ,
        BX_ARCHIVE_CODEC_INPUT_ZSTD,
    } kind;
    struct bx_fd_input source;
    unsigned char decode_buffer[8192];
    size_t decode_pos;
    size_t decode_len;
    bool source_eof;
    bool decoder_end;
    void* codec_reader;
    size_t thread_count;
    const struct bx_archive_codec* required_codec;
    bool checked_mode;
    bool detect_all;
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

bool bx_archive_codec_level_valid(const struct bx_archive_codec* codec, int level) {
    if (codec == bx_archive_codec_gzip() || codec == bx_archive_codec_bzip2())
        return level >= 1 && level <= 9;
    if (codec == bx_archive_codec_xz())
        return level >= 0 && level <= 9;
    if (codec == bx_archive_codec_zstd())
        return level >= 1 && level <= 19;
    return false;
}

static bool bx_archive_codec_encode_level(const struct bx_archive_codec* codec,
                                          const struct bx_archive_codec_encode_options* options,
                                          int* level,
                                          struct bx_diag_ctx* diag) {
    *level = -1;
    if (options == NULL || !options->level_set)
        return true;
    if (!bx_archive_codec_level_valid(codec, options->level)) {
        bx_diag(diag, "invalid %s compression level %d", bx_archive_codec_name(codec), options->level);
        return false;
    }
    *level = options->level;
    return true;
}

bool bx_archive_codec_run_encode_stream(const struct bx_archive_codec* codec,
                                        const struct bx_archive_codec_encode_options* options,
                                        bx_archive_codec_stream_producer_fn producer,
                                        void* producer_user,
                                        const struct bx_archive_codec_stream_sink* output_sink,
                                        struct bx_diag_ctx* diag) {
    int level;
    if (!bx_archive_codec_encode_level(codec, options, &level, diag))
        return false;
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
                                                 level,
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
                                                  level,
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
                                               level,
                                               options != NULL ? options->thread_count : 0u,
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
                                                 level,
                                                 diag);
    }
    bx_diag(diag, "unsupported archive codec '%s'", bx_archive_codec_name(codec));
    return false;
}

bool bx_archive_codec_run_encode_mt_stream(const struct bx_archive_codec* codec,
                                           const struct bx_archive_codec_encode_options* options,
                                           bx_archive_codec_stream_producer_fn producer,
                                           void* producer_user,
                                           const struct bx_archive_codec_stream_sink* output_sink,
                                           const struct bx_archive_codec_mt_options* mt_options,
                                           struct bx_diag_ctx* diag) {
    int level;
    if (!bx_archive_codec_encode_level(codec, options, &level, diag))
        return false;
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
                                                    level,
                                                    diag);
    }
    bx_diag(diag, "unsupported archive codec '%s'", bx_archive_codec_name(codec));
    return false;
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

bool bx_archive_codec_detect_fd(int fd, const struct bx_archive_codec** codec_out) {
    struct bx_fd_input source = BX_FD_INPUT_INIT;
    unsigned char magic[6];
    *codec_out = NULL;
    if (bx_fd_input_init(&source, fd, BX_FD_INPUT_BORROWED) != 0)
        return false;
    ssize_t count = bx_fd_input_pread(&source, magic, sizeof(magic), 0, bx_archive_temp_signal_fd());
    bx_fd_input_close(&source);
    if (count < 0)
        return false;
    *codec_out = bx_archive_codec_detect_magic(magic, (size_t)count);
    return true;
}

static bool bx_archive_codec_input_start_decoder(struct bx_archive_codec_input* input, const struct bx_archive_codec* codec, struct bx_diag_ctx* diag) {
    input->required_codec = codec;
    input->checked_mode = true;
    if (codec == bx_archive_codec_bzip2()) {
        input->kind = BX_ARCHIVE_CODEC_INPUT_BZIP2;
        return bx_archive_bzip2_reader_open((struct bx_archive_bzip2_reader**)&input->codec_reader, diag);
    }
    if (codec == bx_archive_codec_xz()) {
        input->kind = BX_ARCHIVE_CODEC_INPUT_XZ;
        return bx_archive_xz_reader_open((struct bx_archive_xz_reader**)&input->codec_reader,
                                         input->thread_count,
                                         diag);
    }
    if (codec == bx_archive_codec_zstd()) {
        input->kind = BX_ARCHIVE_CODEC_INPUT_ZSTD;
        return bx_archive_zstd_reader_open((struct bx_archive_zstd_reader**)&input->codec_reader, diag);
    }
    input->kind = BX_ARCHIVE_CODEC_INPUT_GZIP;
    return bx_archive_gzip_reader_open((struct bx_archive_gzip_reader**)&input->codec_reader, diag);
}

static bool bx_archive_codec_input_check_mode(struct bx_archive_codec_input* input, struct bx_diag_ctx* diag) {
    if (input->checked_mode)
        return true;
    size_t needed = input->detect_all ? 6u : 2u;
    while (input->decode_len < needed && !input->source_eof) {
        ssize_t count = bx_fd_input_read(&input->source, input->decode_buffer + input->decode_len, sizeof(input->decode_buffer) - input->decode_len, bx_archive_temp_signal_fd());
        if (count < 0) {
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        input->source_eof = count == 0;
        input->decode_len += (size_t)count;
    }
    const struct bx_archive_codec* codec = bx_archive_codec_detect_magic(input->decode_buffer, input->decode_len);
    if (!input->detect_all && codec != bx_archive_codec_gzip())
        codec = NULL;
    if (input->required_codec == bx_archive_codec_gzip() && codec != bx_archive_codec_gzip()) {
        bx_diag(diag, "archive is not in gzip format");
        return false;
    }
    if (codec)
        return bx_archive_codec_input_start_decoder(input, codec, diag);
    input->checked_mode = true;
    return true;
}

static bool bx_archive_codec_input_open_source(struct bx_archive_codec_input** input_out,
                                               int fd,
                                               enum bx_fd_input_ownership ownership,
                                               const struct bx_archive_codec* required_codec,
                                               enum bx_archive_codec_seek_mode seek_mode,
                                               size_t thread_count,
                                               struct bx_diag_ctx* diag) {
    struct bx_archive_codec_input* input;

    if (input_out == NULL) {
        bx_diag(diag, "invalid archive codec reader configuration");
        return false;
    }

    *input_out = NULL;
    input = xmalloc(sizeof(*input));
    memset(input, 0, sizeof(*input));
    input->source = (struct bx_fd_input)BX_FD_INPUT_INIT;
    input->force_seek = seek_mode == BX_ARCHIVE_CODEC_SEEK_FORCE;
    input->thread_count = thread_count;
    if (bx_fd_input_init(&input->source, fd, ownership) != 0) {
        int error = errno;
        if (ownership == BX_FD_INPUT_OWNED)
            close(fd);
        free(input);
        bx_diag(diag, "read error: %s", strerror(error));
        return false;
    }
    fd = input->source.fd;

    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0) {
        off_t offset = lseek(fd, 0, SEEK_CUR);
        input->detect_all = required_codec == NULL;
        if (offset >= 0 && offset <= st.st_size) {
            input->plain_seekable = seek_mode != BX_ARCHIVE_CODEC_SEEK_DISABLE;
            input->plain_size = (uint64_t)(st.st_size - offset);
        }
    }
    input->required_codec = required_codec;
    if (required_codec == bx_archive_codec_bzip2() || required_codec == bx_archive_codec_xz() || required_codec == bx_archive_codec_zstd()) {
        if (!bx_archive_codec_input_start_decoder(input, required_codec, diag)) {
            bx_archive_codec_input_close(input);
            return false;
        }
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
    return bx_archive_codec_input_open_source(input_out, fd, BX_FD_INPUT_DUPLICATE_BORROWED, required_codec, BX_ARCHIVE_CODEC_SEEK_AUTO, 0u, diag);
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
        return bx_archive_codec_input_open_source(input_out, STDIN_FILENO, BX_FD_INPUT_DUPLICATE_BORROWED, options->required_codec, options->seek_mode, options->thread_count, diag);
    }
    else {
        fd = bx_fd_open_cloexec(options->archive_path, O_RDONLY, 0);
        if (fd < 0) {
            bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
            return false;
        }
    }

    return bx_archive_codec_input_open_source(input_out, fd, BX_FD_INPUT_OWNED, options->required_codec, options->seek_mode, options->thread_count, diag);
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
        if (input->kind == BX_ARCHIVE_CODEC_INPUT_GZIP)
            result = bx_archive_gzip_reader_decode(input->codec_reader, &chunk);
        else if (input->kind == BX_ARCHIVE_CODEC_INPUT_BZIP2)
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

bool bx_archive_codec_input_read_some(struct bx_archive_codec_input* input, unsigned char* buffer, size_t len, size_t* nread_out, struct bx_diag_ctx* diag) {
    if (len == 0) {
        *nread_out = 0;
        return true;
    }
    if (bx_archive_temp_pending_signal()) {
        errno = EINTR;
        bx_diag(diag, "read error: %s", strerror(errno));
        return false;
    }
    if (!bx_archive_codec_input_check_mode(input, diag))
        return false;
    if (input->kind != BX_ARCHIVE_CODEC_INPUT_PLAIN)
        return bx_archive_codec_input_read_decoded(input, buffer, len, nread_out, diag);

    if (len > sizeof(input->decode_buffer))
        len = sizeof(input->decode_buffer);
    size_t remaining = input->decode_len - input->decode_pos;
    if (remaining) {
        *nread_out = remaining > len ? len : remaining;
        memcpy(buffer, input->decode_buffer + input->decode_pos, *nread_out);
        input->decode_pos += *nread_out;
    }
    else {
        ssize_t count = input->source_eof ? 0 : bx_fd_input_read(&input->source, buffer, len, bx_archive_temp_signal_fd());
        if (count < 0) {
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        input->source_eof = count == 0;
        *nread_out = (size_t)count;
    }
    input->logical_offset += *nread_out;
    return true;
}

static bool bx_archive_codec_input_skip_direct_seek(struct bx_archive_codec_input* input, size_t* len, struct bx_diag_ctx* diag) {
    if (input->plain_seekable && (input->logical_offset > input->plain_size || *len > input->plain_size - input->logical_offset)) {
        bx_diag(diag, "truncated archive");
        return false;
    }
    size_t buffered = input->decode_len - input->decode_pos;
    if (buffered > *len)
        buffered = *len;
    input->decode_pos += buffered;
    input->logical_offset += buffered;
    *len -= buffered;
    while (*len) {
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        size_t chunk = *len > (size_t)LONG_MAX ? (size_t)LONG_MAX : *len;
        if (lseek(input->source.fd, (off_t)chunk, SEEK_CUR) < 0) {
            if (!input->plain_seekable) {
                input->force_seek = false;
                return true;
            }
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        input->logical_offset += chunk;
        *len -= chunk;
    }
    return true;
}

bool bx_archive_codec_input_skip(struct bx_archive_codec_input* input, size_t len, struct bx_diag_ctx* diag) {
    unsigned char buffer[8192];

    if (len == 0u) {
        return true;
    }
    if (bx_archive_temp_pending_signal()) {
        errno = EINTR;
        bx_diag(diag, "read error: %s", strerror(errno));
        return false;
    }
    if (!bx_archive_codec_input_check_mode(input, diag))
        return false;
    if (input->kind == BX_ARCHIVE_CODEC_INPUT_PLAIN && (input->plain_seekable || input->force_seek)) {
        if (!bx_archive_codec_input_skip_direct_seek(input, &len, diag))
            return false;
    }

    while (len > 0u) {
        size_t nread = 0u;

        if (!bx_archive_codec_input_read_some(input, buffer, len > sizeof(buffer) ? sizeof(buffer) : len, &nread, diag)) {
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

bool bx_archive_codec_input_finish_success(struct bx_archive_codec_input* input, struct bx_diag_ctx* diag) {
    if (bx_archive_temp_pending_signal()) {
        errno = EINTR;
        bx_diag(diag, "read error: %s", strerror(errno));
        return false;
    }
    if (!bx_archive_codec_input_check_mode(input, diag))
        return false;
    if (input->kind == BX_ARCHIVE_CODEC_INPUT_PLAIN && input->plain_seekable)
        return true;
    if (!bx_archive_codec_input_drain_to_eof(input, diag))
        return false;
    /* Gzip permits trailing non-member bytes, but finish still observes source
     * EOF and cancellation through the common input owner. */
    unsigned char discard[8192];
    while (!input->source_eof) {
        ssize_t count = bx_fd_input_read(&input->source, discard, sizeof(discard), bx_archive_temp_signal_fd());
        if (count < 0) {
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        input->source_eof = count == 0;
    }
    return true;
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
    else if (input->kind == BX_ARCHIVE_CODEC_INPUT_GZIP) {
        bx_archive_gzip_reader_close(input->codec_reader);
    }
    bx_fd_input_close(&input->source);
    free(input);
}
