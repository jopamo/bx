#include "applets.h"
#include "crypto/digestsum.h"
#include "crypto/sha256.h"

static void bx_sha256sum_init_adapter(void* ctx) {
    bx_sha256_init((struct bx_sha256_ctx*)ctx);
}

static void bx_sha256sum_update_adapter(void* ctx, const void* data, size_t len) {
    bx_sha256_update((struct bx_sha256_ctx*)ctx, data, len);
}

static void bx_sha256sum_final_adapter(void* ctx, uint8_t* out) {
    bx_sha256_final((struct bx_sha256_ctx*)ctx, out);
}

int bx_sha256sum_main(int argc, char** argv) {
    static const struct bx_digestsum_impl impl = {
        .default_progname = "sha256sum",
        .algorithm_label = "SHA256",
        .digest_size = BX_SHA256_DIGEST_SIZE,
        .ctx_size = sizeof(struct bx_sha256_ctx),
        .init_fn = bx_sha256sum_init_adapter,
        .update_fn = bx_sha256sum_update_adapter,
        .final_fn = bx_sha256sum_final_adapter,
    };

    return bx_digestsum_main(argc, argv, &impl);
}
