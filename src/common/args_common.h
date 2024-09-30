#ifndef BX_COMMON_ARGS_COMMON_H
#define BX_COMMON_ARGS_COMMON_H

#include <stdbool.h>
#include "update_policy.h"

bool bx_args_parse_preserve_list(const char *arg,
                                unsigned *mask,
                                bool set_bits,
                                bool *mode_mentioned_out,
                                char **invalid_token_out);

bool bx_args_parse_update_mode(const char *arg,
                               enum bx_update_mode *mode_out);

#endif /* BX_COMMON_ARGS_COMMON_H */
