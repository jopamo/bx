#ifndef BX_APPLETS_ARCHIVE_ARCHIVE_GZIP_H
#define BX_APPLETS_ARCHIVE_ARCHIVE_GZIP_H

#include <stdbool.h>
#include <stddef.h>

#include "applets/archive/archive_common.h"

bool bx_archive_run_gzip_filter(const struct bx_archive_buffer* input,
                                struct bx_archive_buffer* output,
                                bool decompress,
                                struct bx_diag_ctx* diag);

bool bx_archive_run_gzip_filter_mt(const struct bx_archive_buffer* input,
                                   struct bx_archive_buffer* output,
                                   size_t thread_count,
                                   size_t chunk_size,
                                   struct bx_diag_ctx* diag);

#endif
