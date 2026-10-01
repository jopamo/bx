#ifndef BX_APPLETS_ARCHIVE_ARCHIVE_BZIP2_H
#define BX_APPLETS_ARCHIVE_ARCHIVE_BZIP2_H

#include <stdbool.h>
#include <stddef.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/archive_decode.h"

struct bx_archive_bzip2_stream_sink {
    void* user;
    bool (*write)(void* user, const void* data, size_t len);
};

typedef bool (*bx_archive_bzip2_stream_producer_fn)(void* user,
                                                    const struct bx_archive_bzip2_stream_sink* sink,
                                                    struct bx_diag_ctx* diag);

struct bx_archive_bzip2_reader;

bool bx_archive_run_bzip2_filter(const struct bx_archive_buffer* input,
                                 struct bx_archive_buffer* output,
                                 bool decompress,
                                 struct bx_diag_ctx* diag);

bool bx_archive_run_bzip2_filter_stream(bx_archive_bzip2_stream_producer_fn producer,
                                        void* producer_user,
                                        const struct bx_archive_bzip2_stream_sink* output_sink,
                                        struct bx_diag_ctx* diag);

bool bx_archive_bzip2_reader_open(struct bx_archive_bzip2_reader** reader_out, struct bx_diag_ctx* diag);
enum bx_archive_decode_result bx_archive_bzip2_reader_decode(struct bx_archive_bzip2_reader* reader, struct bx_archive_decode_chunk* chunk);
void bx_archive_bzip2_reader_close(struct bx_archive_bzip2_reader* reader);

#endif
