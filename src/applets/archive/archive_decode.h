#ifndef BX_APPLETS_ARCHIVE_ARCHIVE_DECODE_H
#define BX_APPLETS_ARCHIVE_ARCHIVE_DECODE_H

#include <stdbool.h>
#include <stddef.h>

/* Borrow both buffers for one decoder call. The caller owns refills, EOF,
 * cancellation and buffer lifetimes; decoders never receive a source fd. */
struct bx_archive_decode_chunk {
    const unsigned char* input;
    size_t input_size;
    size_t input_used;
    unsigned char* output;
    size_t output_size;
    size_t output_used;
    bool input_eof;
    const char* error_detail; /* Borrow until the next decoder call. */
};

enum bx_archive_decode_result {
    BX_ARCHIVE_DECODE_ERROR = -1,
    BX_ARCHIVE_DECODE_MORE,
    BX_ARCHIVE_DECODE_END,
};

#endif
