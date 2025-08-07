#include "applets.h"
#include "crypto/digestsum.h"
#include "crypto/md5.h"

static void bx_md5sum_init_adapter(void* ctx) {
    bx_md5_init((struct bx_md5_ctx*)ctx);
}

static void bx_md5sum_update_adapter(void* ctx, const void* data, size_t len) {
    bx_md5_update((struct bx_md5_ctx*)ctx, data, len);
}

static void bx_md5sum_final_adapter(void* ctx, uint8_t* out) {
    bx_md5_final((struct bx_md5_ctx*)ctx, out);
}

int bx_md5sum_main(int argc, char** argv) {
    static const struct bx_digestsum_impl impl = {
        .default_progname = "md5sum",
        .algorithm_label = "MD5",
        .digest_size = BX_MD5_DIGEST_SIZE,
        .ctx_size = sizeof(struct bx_md5_ctx),
        .init_fn = bx_md5sum_init_adapter,
        .update_fn = bx_md5sum_update_adapter,
        .final_fn = bx_md5sum_final_adapter,
    };

    return bx_digestsum_main(argc, argv, &impl);
}
