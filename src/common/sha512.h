#ifndef BX_COMMON_SHA512_H
#define BX_COMMON_SHA512_H

#include <stddef.h>
#include <stdint.h>

#define BX_SHA384_DIGEST_SIZE 48u
#define BX_SHA512_DIGEST_SIZE 64u
#define BX_SHA512_BLOCK_SIZE 128u

struct bx_sha512_ctx {
    uint64_t h[8];
    uint64_t total_len_hi;
    uint64_t total_len_lo;
    uint8_t buffer[BX_SHA512_BLOCK_SIZE];
    size_t buffer_len;
};

void bx_sha384_init(struct bx_sha512_ctx* ctx);
void bx_sha512_init(struct bx_sha512_ctx* ctx);
void bx_sha512_update(struct bx_sha512_ctx* ctx, const void* data, size_t len);
void bx_sha384_final(struct bx_sha512_ctx* ctx, uint8_t out[BX_SHA384_DIGEST_SIZE]);
void bx_sha512_final(struct bx_sha512_ctx* ctx, uint8_t out[BX_SHA512_DIGEST_SIZE]);

#endif /* BX_COMMON_SHA512_H */
