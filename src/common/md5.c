#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common/md5.h"

static uint32_t bx_md5_rotl32(uint32_t value, unsigned shift) {
    return (value << shift) | (value >> (32u - shift));
}

static uint32_t bx_md5_load_le32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0]) |
           ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) |
           ((uint32_t)bytes[3] << 24u);
}

static void bx_md5_store_le32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value);
    out[1] = (uint8_t)(value >> 8u);
    out[2] = (uint8_t)(value >> 16u);
    out[3] = (uint8_t)(value >> 24u);
}

static void bx_md5_transform(struct bx_md5_ctx *ctx, const uint8_t block[BX_MD5_BLOCK_SIZE]) {
    static const uint32_t k[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
        0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
        0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
        0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
        0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
        0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
        0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
        0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
        0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
    };

    static const uint8_t s[64] = {
         7u, 12u, 17u, 22u,  7u, 12u, 17u, 22u,  7u, 12u, 17u, 22u,  7u, 12u, 17u, 22u,
         5u,  9u, 14u, 20u,  5u,  9u, 14u, 20u,  5u,  9u, 14u, 20u,  5u,  9u, 14u, 20u,
         4u, 11u, 16u, 23u,  4u, 11u, 16u, 23u,  4u, 11u, 16u, 23u,  4u, 11u, 16u, 23u,
         6u, 10u, 15u, 21u,  6u, 10u, 15u, 21u,  6u, 10u, 15u, 21u,  6u, 10u, 15u, 21u,
    };

    uint32_t w[16];
    uint32_t a = ctx->a;
    uint32_t b = ctx->b;
    uint32_t c = ctx->c;
    uint32_t d = ctx->d;

    for (size_t i = 0; i < 16u; i++) {
        w[i] = bx_md5_load_le32(block + (i * 4u));
    }

    for (size_t i = 0; i < 64u; i++) {
        uint32_t f;
        uint32_t g;

        if (i < 16u) {
            f = (b & c) | ((~b) & d);
            g = (uint32_t)i;
        } else if (i < 32u) {
            f = (d & b) | ((~d) & c);
            g = (uint32_t)((5u * i + 1u) & 15u);
        } else if (i < 48u) {
            f = b ^ c ^ d;
            g = (uint32_t)((3u * i + 5u) & 15u);
        } else {
            f = c ^ (b | (~d));
            g = (uint32_t)((7u * i) & 15u);
        }

        uint32_t next_d = d;
        d = c;
        c = b;
        b = b + bx_md5_rotl32(a + f + k[i] + w[g], s[i]);
        a = next_d;
    }

    ctx->a += a;
    ctx->b += b;
    ctx->c += c;
    ctx->d += d;
}

void bx_md5_init(struct bx_md5_ctx *ctx) {
    ctx->a = 0x67452301u;
    ctx->b = 0xefcdab89u;
    ctx->c = 0x98badcfeu;
    ctx->d = 0x10325476u;
    ctx->total_len = 0u;
    ctx->buffer_len = 0u;
}

void bx_md5_update(struct bx_md5_ctx *ctx, const void *data_, size_t len) {
    const uint8_t *data = (const uint8_t *)data_;

    ctx->total_len += (uint64_t)len;

    if (ctx->buffer_len > 0u) {
        size_t take = BX_MD5_BLOCK_SIZE - ctx->buffer_len;
        if (take > len) {
            take = len;
        }

        memcpy(ctx->buffer + ctx->buffer_len, data, take);
        ctx->buffer_len += take;
        data += take;
        len -= take;

        if (ctx->buffer_len == BX_MD5_BLOCK_SIZE) {
            bx_md5_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0u;
        }
    }

    while (len >= BX_MD5_BLOCK_SIZE) {
        bx_md5_transform(ctx, data);
        data += BX_MD5_BLOCK_SIZE;
        len -= BX_MD5_BLOCK_SIZE;
    }

    if (len > 0u) {
        memcpy(ctx->buffer, data, len);
        ctx->buffer_len = len;
    }
}

void bx_md5_final(struct bx_md5_ctx *ctx, uint8_t out[BX_MD5_DIGEST_SIZE]) {
    uint8_t pad[64] = {0};
    uint8_t length_le[8];
    uint64_t total_bits = ctx->total_len * 8u;
    size_t pad_len;

    pad[0] = 0x80u;
    for (size_t i = 0; i < 8u; i++) {
        length_le[i] = (uint8_t)(total_bits >> (i * 8u));
    }

    pad_len = (ctx->buffer_len < 56u)
              ? (56u - ctx->buffer_len)
              : (64u + 56u - ctx->buffer_len);

    bx_md5_update(ctx, pad, pad_len);
    bx_md5_update(ctx, length_le, sizeof(length_le));

    bx_md5_store_le32(out + 0u, ctx->a);
    bx_md5_store_le32(out + 4u, ctx->b);
    bx_md5_store_le32(out + 8u, ctx->c);
    bx_md5_store_le32(out + 12u, ctx->d);
}
