#ifndef BX_COMMON_SIZE_PARSE_H
#define BX_COMMON_SIZE_PARSE_H

#include <stdbool.h>
#include <inttypes.h>

bool bx_dd_parse_u64(const char* text, uintmax_t* value_out);
bool bx_dd_parse_size(const char* text, uintmax_t* value_out);

#endif /* BX_COMMON_SIZE_PARSE_H */
