#ifndef BX_COMMON_SHA256_H
#define BX_COMMON_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define BX_SHA224_DIGEST_SIZE 28u
#define BX_SHA256_DIGEST_SIZE 32u
#define BX_SHA256_BLOCK_SIZE 64u

struct bx_sha256_ctx {
    uint32_t h[8];
    uint64_t total_len;
    uint8_t buffer[BX_SHA256_BLOCK_SIZE];
    size_t buffer_len;
};

void bx_sha224_init(struct bx_sha256_ctx* ctx);
void bx_sha256_init(struct bx_sha256_ctx* ctx);
void bx_sha256_update(struct bx_sha256_ctx* ctx, const void* data, size_t len);
void bx_sha224_final(struct bx_sha256_ctx* ctx, uint8_t out[BX_SHA224_DIGEST_SIZE]);
void bx_sha256_final(struct bx_sha256_ctx* ctx, uint8_t out[BX_SHA256_DIGEST_SIZE]);

#endif /* BX_COMMON_SHA256_H */
