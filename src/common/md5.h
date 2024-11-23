#ifndef BX_COMMON_MD5_H
#define BX_COMMON_MD5_H

#include <stddef.h>
#include <stdint.h>

#define BX_MD5_DIGEST_SIZE 16u
#define BX_MD5_BLOCK_SIZE 64u

struct bx_md5_ctx {
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint64_t total_len;
    uint8_t buffer[BX_MD5_BLOCK_SIZE];
    size_t buffer_len;
};

void bx_md5_init(struct bx_md5_ctx* ctx);
void bx_md5_update(struct bx_md5_ctx* ctx, const void* data, size_t len);
void bx_md5_final(struct bx_md5_ctx* ctx, uint8_t out[BX_MD5_DIGEST_SIZE]);

#endif /* BX_COMMON_MD5_H */
