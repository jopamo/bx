#ifndef BX_SEARCH_RG_TEXT_H
#define BX_SEARCH_RG_TEXT_H

#include <stdbool.h>
#include <stddef.h>

struct search_opts;

enum bx_rg_encoding_mode {
    BX_RG_ENCODING_AUTO = 0,
    BX_RG_ENCODING_NONE,
    BX_RG_ENCODING_EXPLICIT,
};

bool bx_rg_parse_encoding_name(const char *progname, const char *name,
                               enum bx_rg_encoding_mode *mode_out,
                               char **encoding_name_out);

bool bx_rg_decode_buffer(enum bx_rg_encoding_mode mode,
                         const char *encoding_name,
                         const unsigned char *input,
                         size_t input_len,
                         unsigned char **output,
                         size_t *output_len);

size_t bx_rg_record_match_len(const unsigned char *buf, size_t len,
                              char delimiter, bool crlf_enabled);
size_t bx_rg_trim_leading_ascii_space(const unsigned char *buf, size_t len);
bool bx_rg_match_has_word_boundaries(const unsigned char *buf, size_t len,
                                     size_t start, size_t end,
                                     bool unicode_mode);

#endif
