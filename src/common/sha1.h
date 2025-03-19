#ifndef BX_COMMON_SHA1_H
#define BX_COMMON_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define BX_SHA1_DIGEST_SIZE 20u
#define BX_SHA1_BLOCK_SIZE 64u

struct bx_sha1_ctx {
    uint32_t h0;
    uint32_t h1;
    uint32_t h2;
    uint32_t h3;
    uint32_t h4;
    uint64_t total_len;
    uint8_t buffer[BX_SHA1_BLOCK_SIZE];
    size_t buffer_len;
};

void bx_sha1_init(struct bx_sha1_ctx* ctx);
void bx_sha1_update(struct bx_sha1_ctx* ctx, const void* data, size_t len);
void bx_sha1_final(struct bx_sha1_ctx* ctx, uint8_t out[BX_SHA1_DIGEST_SIZE]);

#endif /* BX_COMMON_SHA1_H */
