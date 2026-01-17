#ifndef BX_APPLETS_ARCHIVE_ARCHIVE_GZIP_H
#define BX_APPLETS_ARCHIVE_ARCHIVE_GZIP_H

#include <stdbool.h>
#include <stddef.h>

#include "applets/archive/archive_common.h"

struct bx_archive_gzip_stream_sink {
    void* user;
    bool (*write)(void* user, const void* data, size_t len);
};

typedef bool (*bx_archive_gzip_stream_producer_fn)(void* user,
                                                   const struct bx_archive_gzip_stream_sink* sink,
                                                   struct bx_diag_ctx* diag);

bool bx_archive_run_gzip_filter(const struct bx_archive_buffer* input,
                                struct bx_archive_buffer* output,
                                bool decompress,
                                struct bx_diag_ctx* diag);

bool bx_archive_run_gzip_filter_stream(bx_archive_gzip_stream_producer_fn producer,
                                       void* producer_user,
                                       const struct bx_archive_gzip_stream_sink* output_sink,
                                       struct bx_diag_ctx* diag);

bool bx_archive_run_gzip_filter_mt_stream(bx_archive_gzip_stream_producer_fn producer,
                                          void* producer_user,
                                          const struct bx_archive_gzip_stream_sink* output_sink,
                                          size_t thread_count,
                                          size_t chunk_size,
                                          size_t max_inflight_chunks,
                                          struct bx_diag_ctx* diag);

#endif
