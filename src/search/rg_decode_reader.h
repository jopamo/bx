#ifndef BX_SEARCH_RG_DECODE_READER_H
#define BX_SEARCH_RG_DECODE_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "rg_text.h"

/*
 * Wrap SOURCE in an incremental UTF-8 decoder. A zero input_limit means that
 * total source size is unbounded. When close_source is true, closing the
 * returned stream also closes SOURCE.
 */
FILE *bx_rg_decode_reader_open(FILE *source,
                               bool close_source,
                               enum bx_rg_encoding_mode mode,
                               const char *encoding_name,
                               size_t input_limit);

#endif
