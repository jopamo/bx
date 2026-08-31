#ifndef BX_CRYPTO_CRC32B_H
#define BX_CRYPTO_CRC32B_H

#include <stddef.h>
#include <stdint.h>

struct bx_crc32b_ctx {
    uint32_t crc;
};

void bx_crc32b_init(struct bx_crc32b_ctx* ctx);
void bx_crc32b_update(struct bx_crc32b_ctx* ctx, const void* data, size_t len);
uint32_t bx_crc32b_final(const struct bx_crc32b_ctx* ctx);

#endif /* BX_CRYPTO_CRC32B_H */
