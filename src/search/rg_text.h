#ifndef BX_SEARCH_RG_TEXT_H
#define BX_SEARCH_RG_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct search_opts;

enum bx_rg_encoding_mode {
    BX_RG_ENCODING_AUTO = 0,
    BX_RG_ENCODING_NONE,
    BX_RG_ENCODING_EXPLICIT,
};

bool bx_rg_parse_encoding_name(const char *progname, const char *name,
                               enum bx_rg_encoding_mode *mode_out,
                               char **encoding_name_out);
bool bx_rg_encoding_is_utf8(const char *name);

bool bx_rg_decode_buffer_limited(enum bx_rg_encoding_mode mode,
                                 const char *encoding_name,
                                 const unsigned char *input,
                                 size_t input_len,
                                 size_t output_limit,
                                 unsigned char **output,
                                 size_t *output_len);
bool bx_rg_decode_stream_limited(FILE *input,
                                 enum bx_rg_encoding_mode mode,
                                 const char *encoding_name,
                                 size_t input_limit,
                                 size_t output_limit,
                                 unsigned char **output,
                                 size_t *output_len);
bool bx_rg_locale_is_utf8(void);
uint32_t bx_rg_locale_uppercase_codepoint(uint32_t cp);
bool bx_rg_decode_utf8_codepoint(const unsigned char *buf, size_t len,
                                 size_t *consumed_out, uint32_t *cp_out);
bool bx_rg_decode_prev_utf8(const unsigned char *buf, size_t end, uint32_t *cp_out);
bool bx_rg_decode_next_utf8(const unsigned char *buf, size_t len, size_t start,
                            uint32_t *cp_out);

size_t bx_rg_record_match_len(const unsigned char *buf, size_t len,
                              char delimiter, bool crlf_enabled);
size_t bx_rg_trim_leading_ascii_space(const unsigned char *buf, size_t len);
bool bx_rg_match_has_word_boundaries(const unsigned char *buf, size_t len,
                                     size_t start, size_t end,
                                     bool unicode_mode);
bool bx_rg_match_has_locale_word_boundaries_utf8(const unsigned char *buf, size_t len,
                                                 size_t start, size_t end);

#endif
