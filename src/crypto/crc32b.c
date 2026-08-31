#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto/crc32b.h"
#include "crypto/crc32b_internal.h"

static uint32_t bx_crc32b_table[256];
static pthread_once_t bx_crc32b_table_once = PTHREAD_ONCE_INIT;

static void bx_crc32b_make_table(void) {
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t crc = i;
        for (unsigned bit = 0; bit < 8u; bit++) {
            crc = (crc >> 1u) ^ ((crc & 1u) != 0u ? 0xedb88320u : 0u);
        }
        bx_crc32b_table[i] = crc;
    }
}

void bx_crc32b_init(struct bx_crc32b_ctx* ctx) {
    ctx->crc = UINT32_MAX;
}

void bx_crc32b_update(struct bx_crc32b_ctx* ctx, const void* data_, size_t len) {
    const uint8_t* data = (const uint8_t*)data_;

    if (bx_crc32b_arm64_update(&ctx->crc, data, len)) {
        return;
    }

    pthread_once(&bx_crc32b_table_once, bx_crc32b_make_table);
    uint32_t crc = ctx->crc;
    for (size_t i = 0; i < len; i++) {
        crc = bx_crc32b_table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8u);
    }
    ctx->crc = crc;
}

uint32_t bx_crc32b_final(const struct bx_crc32b_ctx* ctx) {
    return ctx->crc ^ UINT32_MAX;
}
