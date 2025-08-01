#include "applets.h"
#include "lib/digestsum.h"
#include "lib/sha1.h"

static void bx_sha1sum_init_adapter(void* ctx) {
    bx_sha1_init((struct bx_sha1_ctx*)ctx);
}

static void bx_sha1sum_update_adapter(void* ctx, const void* data, size_t len) {
    bx_sha1_update((struct bx_sha1_ctx*)ctx, data, len);
}

static void bx_sha1sum_final_adapter(void* ctx, uint8_t* out) {
    bx_sha1_final((struct bx_sha1_ctx*)ctx, out);
}

int bx_sha1sum_main(int argc, char** argv) {
    static const struct bx_digestsum_impl impl = {
        .default_progname = "sha1sum",
        .algorithm_label = "SHA1",
        .digest_size = BX_SHA1_DIGEST_SIZE,
        .ctx_size = sizeof(struct bx_sha1_ctx),
        .init_fn = bx_sha1sum_init_adapter,
        .update_fn = bx_sha1sum_update_adapter,
        .final_fn = bx_sha1sum_final_adapter,
    };

    return bx_digestsum_main(argc, argv, &impl);
}
