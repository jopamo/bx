#ifndef BX_APPLETS_BASE_ENV_SPLIT_H
#define BX_APPLETS_BASE_ENV_SPLIT_H

#include <stdbool.h>

#include "bx/diag.h"

struct bx_env_split_result {
    char **argv;
    int argc;
    int owned_word_count;
};

bool bx_env_split_parse(
    const char *input,
    int original_argc,
    char **original_argv,
    int original_optind,
    bool debug,
    struct bx_diag_ctx *diag,
    struct bx_env_split_result *result);
void bx_env_split_result_destroy(struct bx_env_split_result *result);

#endif
