#ifndef BX_APPLETS_ARCHIVE_ARCHIVE_ZSTD_H
#define BX_APPLETS_ARCHIVE_ARCHIVE_ZSTD_H

#include <stdbool.h>
#include <stddef.h>

#include "applets/archive/archive_common.h"

struct bx_archive_zstd_stream_sink {
    void* user;
    bool (*write)(void* user, const void* data, size_t len);
};

typedef bool (*bx_archive_zstd_stream_producer_fn)(void* user,
                                                   const struct bx_archive_zstd_stream_sink* sink,
                                                   struct bx_diag_ctx* diag);

struct bx_archive_zstd_reader;

bool bx_archive_run_zstd_filter(const struct bx_archive_buffer* input,
                                struct bx_archive_buffer* output,
                                bool decompress,
                                struct bx_diag_ctx* diag);

bool bx_archive_run_zstd_filter_stream(bx_archive_zstd_stream_producer_fn producer,
                                       void* producer_user,
                                       const struct bx_archive_zstd_stream_sink* output_sink,
                                       struct bx_diag_ctx* diag);

bool bx_archive_zstd_reader_open(struct bx_archive_zstd_reader** reader_out,
                                 int fd,
                                 struct bx_diag_ctx* diag);
bool bx_archive_zstd_reader_read_some(struct bx_archive_zstd_reader* reader,
                                      unsigned char* buffer,
                                      size_t len,
                                      size_t* nread_out,
                                      struct bx_diag_ctx* diag);
void bx_archive_zstd_reader_close(struct bx_archive_zstd_reader* reader);

#endif
