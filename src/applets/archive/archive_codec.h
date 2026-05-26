#ifndef BX_APPLETS_ARCHIVE_ARCHIVE_CODEC_H
#define BX_APPLETS_ARCHIVE_ARCHIVE_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "applets/archive/archive_common.h"

struct bx_archive_codec;
struct bx_archive_codec_input;

struct bx_archive_codec_stream_sink {
    void* user;
    bool (*write)(void* user, const void* data, size_t len);
};

typedef bool (*bx_archive_codec_stream_producer_fn)(void* user,
                                                    const struct bx_archive_codec_stream_sink* sink,
                                                    struct bx_diag_ctx* diag);

struct bx_archive_codec_mt_options {
    size_t thread_count;
    size_t chunk_size;
    size_t max_inflight_chunks;
};

struct bx_archive_codec_input_options {
    const char* archive_path;
    const struct bx_archive_codec* required_codec;
};

const struct bx_archive_codec* bx_archive_codec_none(void);
const struct bx_archive_codec* bx_archive_codec_gzip(void);
const struct bx_archive_codec* bx_archive_codec_bzip2(void);
const struct bx_archive_codec* bx_archive_codec_xz(void);
const struct bx_archive_codec* bx_archive_codec_zstd(void);

const char* bx_archive_codec_name(const struct bx_archive_codec* codec);
bool bx_archive_codec_supports_mt_encode(const struct bx_archive_codec* codec);
bool bx_archive_codec_matches_path_suffix(const struct bx_archive_codec* codec, const char* path);
const struct bx_archive_codec* bx_archive_codec_detect_path_suffix(const char* path);
const struct bx_archive_codec* bx_archive_codec_detect_fd(int fd);

bool bx_archive_codec_encode_buffer(const struct bx_archive_codec* codec,
                                    const struct bx_archive_buffer* input,
                                    struct bx_archive_buffer* output,
                                    struct bx_diag_ctx* diag);
bool bx_archive_codec_decode_buffer(const struct bx_archive_codec* codec,
                                    const struct bx_archive_buffer* input,
                                    struct bx_archive_buffer* output,
                                    struct bx_diag_ctx* diag);

bool bx_archive_codec_run_encode_stream(const struct bx_archive_codec* codec,
                                        bx_archive_codec_stream_producer_fn producer,
                                        void* producer_user,
                                        const struct bx_archive_codec_stream_sink* output_sink,
                                        struct bx_diag_ctx* diag);
bool bx_archive_codec_run_encode_mt_stream(const struct bx_archive_codec* codec,
                                           bx_archive_codec_stream_producer_fn producer,
                                           void* producer_user,
                                           const struct bx_archive_codec_stream_sink* output_sink,
                                           const struct bx_archive_codec_mt_options* mt_options,
                                           struct bx_diag_ctx* diag);

bool bx_archive_codec_input_open(struct bx_archive_codec_input** input_out,
                                 const struct bx_archive_codec_input_options* options,
                                 struct bx_diag_ctx* diag);
bool bx_archive_codec_input_open_fd(struct bx_archive_codec_input** input_out,
                                    int fd,
                                    const struct bx_archive_codec* required_codec,
                                    struct bx_diag_ctx* diag);
bool bx_archive_codec_input_read_some(struct bx_archive_codec_input* input,
                                      unsigned char* buffer,
                                      size_t len,
                                      size_t* nread_out,
                                      struct bx_diag_ctx* diag);
bool bx_archive_codec_input_skip(struct bx_archive_codec_input* input,
                                 size_t len,
                                 struct bx_diag_ctx* diag);
bool bx_archive_codec_input_finish_success(struct bx_archive_codec_input* input,
                                           struct bx_diag_ctx* diag);
uint64_t bx_archive_codec_input_total_bytes_read(const struct bx_archive_codec_input* input);
void bx_archive_codec_input_close(struct bx_archive_codec_input* input);

#endif
