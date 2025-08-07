#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "crypto/sha1.h"

static uint32_t bx_sha1_rotl32(uint32_t value, unsigned shift) {
    return (value << shift) | (value >> (32u - shift));
}

static uint32_t bx_sha1_load_be32(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) | ((uint32_t)bytes[2] << 8u) | ((uint32_t)bytes[3]);
}

static void bx_sha1_store_be32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24u);
    out[1] = (uint8_t)(value >> 16u);
    out[2] = (uint8_t)(value >> 8u);
    out[3] = (uint8_t)value;
}

static void bx_sha1_transform(struct bx_sha1_ctx* ctx, const uint8_t block[BX_SHA1_BLOCK_SIZE]) {
    uint32_t w[80];
    uint32_t a = ctx->h0;
    uint32_t b = ctx->h1;
    uint32_t c = ctx->h2;
    uint32_t d = ctx->h3;
    uint32_t e = ctx->h4;

    for (size_t i = 0; i < 16u; i++) {
        w[i] = bx_sha1_load_be32(block + (i * 4u));
    }
    for (size_t i = 16u; i < 80u; i++) {
        w[i] = bx_sha1_rotl32(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1u);
    }

    for (size_t i = 0; i < 80u; i++) {
        uint32_t f;
        uint32_t k;

        if (i < 20u) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        }
        else if (i < 40u) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        }
        else if (i < 60u) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        }
        else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }

        uint32_t temp = bx_sha1_rotl32(a, 5u) + f + e + k + w[i];
        e = d;
        d = c;
        c = bx_sha1_rotl32(b, 30u);
        b = a;
        a = temp;
    }

    ctx->h0 += a;
    ctx->h1 += b;
    ctx->h2 += c;
    ctx->h3 += d;
    ctx->h4 += e;
}

void bx_sha1_init(struct bx_sha1_ctx* ctx) {
    ctx->h0 = 0x67452301u;
    ctx->h1 = 0xEFCDAB89u;
    ctx->h2 = 0x98BADCFEu;
    ctx->h3 = 0x10325476u;
    ctx->h4 = 0xC3D2E1F0u;
    ctx->total_len = 0u;
    ctx->buffer_len = 0u;
}

void bx_sha1_update(struct bx_sha1_ctx* ctx, const void* data_, size_t len) {
    const uint8_t* data = (const uint8_t*)data_;

    ctx->total_len += (uint64_t)len;

    if (ctx->buffer_len > 0u) {
        size_t take = BX_SHA1_BLOCK_SIZE - ctx->buffer_len;
        if (take > len) {
            take = len;
        }

        memcpy(ctx->buffer + ctx->buffer_len, data, take);
        ctx->buffer_len += take;
        data += take;
        len -= take;

        if (ctx->buffer_len == BX_SHA1_BLOCK_SIZE) {
            bx_sha1_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0u;
        }
    }

    while (len >= BX_SHA1_BLOCK_SIZE) {
        bx_sha1_transform(ctx, data);
        data += BX_SHA1_BLOCK_SIZE;
        len -= BX_SHA1_BLOCK_SIZE;
    }

    if (len > 0u) {
        memcpy(ctx->buffer, data, len);
        ctx->buffer_len = len;
    }
}

void bx_sha1_final(struct bx_sha1_ctx* ctx, uint8_t out[BX_SHA1_DIGEST_SIZE]) {
    uint8_t pad[64] = {0};
    uint8_t length_be[8];
    uint64_t total_bits = ctx->total_len * 8u;
    size_t pad_len;

    pad[0] = 0x80u;
    for (size_t i = 0; i < 8u; i++) {
        length_be[7u - i] = (uint8_t)(total_bits >> (i * 8u));
    }

    pad_len = (ctx->buffer_len < 56u) ? (56u - ctx->buffer_len) : (64u + 56u - ctx->buffer_len);

    bx_sha1_update(ctx, pad, pad_len);
    bx_sha1_update(ctx, length_be, sizeof(length_be));

    bx_sha1_store_be32(out + 0u, ctx->h0);
    bx_sha1_store_be32(out + 4u, ctx->h1);
    bx_sha1_store_be32(out + 8u, ctx->h2);
    bx_sha1_store_be32(out + 12u, ctx->h3);
    bx_sha1_store_be32(out + 16u, ctx->h4);
}
