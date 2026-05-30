#ifndef BX_COMMON_SIZE_PARSE_H
#define BX_COMMON_SIZE_PARSE_H

#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>

enum bx_size_suffix_parse_result {
    BX_SIZE_SUFFIX_PARSE_OK = 0,
    BX_SIZE_SUFFIX_PARSE_INVALID,
    BX_SIZE_SUFFIX_PARSE_TOO_LARGE,
};

enum bx_size_unit_label_style {
    BX_SIZE_UNIT_LABEL_SI_LOWER_K = 0,
    BX_SIZE_UNIT_LABEL_IEC_PREFIX,
    BX_SIZE_UNIT_LABEL_IEC_I_SUFFIX,
};

bool bx_size_parse_uint(const char* text, uintmax_t* value_out);
bool bx_size_parse_signed_count(const char* text, intmax_t* value_out);
/* Shared suffix multiplier ownership lives here; applets keep only grammar policy. */
bool bx_size_suffix_prefix_power(char suffix, unsigned int* power_out);
const char* bx_size_unit_label(enum bx_size_unit_label_style style, unsigned int power);
enum bx_size_suffix_parse_result bx_size_suffix_multiplier_result(const char* suffix, uintmax_t* multiplier_out);
bool bx_size_suffix_multiplier(const char* suffix, uintmax_t* multiplier_out);
bool bx_size_parse_scaled_uint(const char* text, uintmax_t* value_out);
void bx_size_format_human_ceil(uintmax_t value, uintmax_t base, const char* suffixes, char* buffer, size_t buffer_size);
void bx_size_format_human_round(uintmax_t value, uintmax_t base, const char* suffixes, bool include_base_suffix, char* buffer, size_t buffer_size);
void bx_size_format_decimal_rate(double bytes_per_sec, char* buffer, size_t buffer_size);
/* Applet-specific prefixes and legacy +/- count spellings stay applet-local. */
bool bx_size_parse_scaled_count(const char* text, intmax_t* value_out);
bool bx_size_parse_block_size(const char* text, uintmax_t* value_out);

bool bx_dd_parse_u64(const char* text, uintmax_t* value_out);
bool bx_dd_parse_size(const char* text, uintmax_t* value_out);

#endif /* BX_COMMON_SIZE_PARSE_H */
