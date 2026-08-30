#ifndef BX_LIB_JQ_PCRE2_BACKEND_H
#define BX_LIB_JQ_PCRE2_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

#include <pcre2.h>

#include "jv.h"

jv bx_jq_pcre2_match(jv input,
                     jv regex,
                     int test,
                     uint32_t compile_options,
                     uint32_t match_options,
                     bool global,
                     bool longest);

#endif /* BX_LIB_JQ_PCRE2_BACKEND_H */
