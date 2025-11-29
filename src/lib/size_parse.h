#ifndef BX_COMMON_SIZE_PARSE_H
#define BX_COMMON_SIZE_PARSE_H

#include <stdbool.h>
#include <inttypes.h>

bool bx_size_parse_uint(const char* text, uintmax_t* value_out);
bool bx_size_parse_signed_count(const char* text, intmax_t* value_out);
bool bx_size_suffix_multiplier(const char* suffix, uintmax_t* multiplier_out);
/* Applet-specific prefixes and legacy +/- count spellings stay applet-local. */
bool bx_size_parse_scaled_count(const char* text, intmax_t* value_out);
bool bx_size_parse_block_size(const char* text, uintmax_t* value_out);

bool bx_dd_parse_u64(const char* text, uintmax_t* value_out);
bool bx_dd_parse_size(const char* text, uintmax_t* value_out);

#endif /* BX_COMMON_SIZE_PARSE_H */
