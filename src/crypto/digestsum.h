#ifndef BX_COMMON_DIGESTSUM_H
#define BX_COMMON_DIGESTSUM_H

#include <stddef.h>

#include "crypto/digest_util.h"

struct bx_digestsum_impl {
    const char* default_progname;
    const char* algorithm_label;
    size_t digest_size;
    size_t ctx_size;
    bx_digest_init_fn init_fn;
    bx_digest_update_fn update_fn;
    bx_digest_final_fn final_fn;
};

int bx_digestsum_main(int argc, char** argv, const struct bx_digestsum_impl* impl);

#endif /* BX_COMMON_DIGESTSUM_H */
