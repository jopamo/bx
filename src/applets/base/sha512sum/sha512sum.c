/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "applets.h"
#include "crypto/digestsum.h"
#include "crypto/sha512.h"

static void sha512sum_init(void* ctx) {
    bx_sha512_init(ctx);
}

static void sha512sum_update(void* ctx, const void* data, size_t len) {
    bx_sha512_update(ctx, data, len);
}

static void sha512sum_final(void* ctx, uint8_t* out) {
    bx_sha512_final(ctx, out);
}

int bx_sha512sum_main(int argc, char** argv) {
    static const struct bx_digestsum_impl impl = {
        .default_progname = "sha512sum",
        .algorithm_label = "SHA512",
        .digest_size = BX_SHA512_DIGEST_SIZE,
        .ctx_size = sizeof(struct bx_sha512_ctx),
        .init_fn = sha512sum_init,
        .update_fn = sha512sum_update,
        .final_fn = sha512sum_final,
    };

    return bx_digestsum_main(argc, argv, &impl);
}
