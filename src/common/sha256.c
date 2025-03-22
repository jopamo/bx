#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common/sha256.h"

static uint32_t bx_sha256_rotr32(uint32_t value, unsigned shift) {
    return (value >> shift) | (value << (32u - shift));
}

static uint32_t bx_sha256_load_be32(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) | ((uint32_t)bytes[2] << 8u) | ((uint32_t)bytes[3]);
}

static void bx_sha256_store_be32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24u);
    out[1] = (uint8_t)(value >> 16u);
    out[2] = (uint8_t)(value >> 8u);
    out[3] = (uint8_t)value;
}

static uint32_t bx_sha256_ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ ((~x) & z);
}

static uint32_t bx_sha256_maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t bx_sha256_big_sigma0(uint32_t x) {
    return bx_sha256_rotr32(x, 2u) ^ bx_sha256_rotr32(x, 13u) ^ bx_sha256_rotr32(x, 22u);
}

static uint32_t bx_sha256_big_sigma1(uint32_t x) {
    return bx_sha256_rotr32(x, 6u) ^ bx_sha256_rotr32(x, 11u) ^ bx_sha256_rotr32(x, 25u);
}

static uint32_t bx_sha256_small_sigma0(uint32_t x) {
    return bx_sha256_rotr32(x, 7u) ^ bx_sha256_rotr32(x, 18u) ^ (x >> 3u);
}

static uint32_t bx_sha256_small_sigma1(uint32_t x) {
    return bx_sha256_rotr32(x, 17u) ^ bx_sha256_rotr32(x, 19u) ^ (x >> 10u);
}

static void bx_sha256_transform(struct bx_sha256_ctx* ctx, const uint8_t block[BX_SHA256_BLOCK_SIZE]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u,
        0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du,
        0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu,
        0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };

    uint32_t w[64];
    uint32_t a = ctx->h[0];
    uint32_t b = ctx->h[1];
    uint32_t c = ctx->h[2];
    uint32_t d = ctx->h[3];
    uint32_t e = ctx->h[4];
    uint32_t f = ctx->h[5];
    uint32_t g = ctx->h[6];
    uint32_t h = ctx->h[7];

    for (size_t i = 0; i < 16u; i++) {
        w[i] = bx_sha256_load_be32(block + (i * 4u));
    }

    for (size_t i = 16u; i < 64u; i++) {
        w[i] = bx_sha256_small_sigma1(w[i - 2u]) + w[i - 7u] + bx_sha256_small_sigma0(w[i - 15u]) + w[i - 16u];
    }

    for (size_t i = 0; i < 64u; i++) {
        uint32_t t1 = h + bx_sha256_big_sigma1(e) + bx_sha256_ch(e, f, g) + k[i] + w[i];
        uint32_t t2 = bx_sha256_big_sigma0(a) + bx_sha256_maj(a, b, c);

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += h;
}

void bx_sha224_init(struct bx_sha256_ctx* ctx) {
    ctx->h[0] = 0xc1059ed8u;
    ctx->h[1] = 0x367cd507u;
    ctx->h[2] = 0x3070dd17u;
    ctx->h[3] = 0xf70e5939u;
    ctx->h[4] = 0xffc00b31u;
    ctx->h[5] = 0x68581511u;
    ctx->h[6] = 0x64f98fa7u;
    ctx->h[7] = 0xbefa4fa4u;
    ctx->total_len = 0u;
    ctx->buffer_len = 0u;
}

void bx_sha256_init(struct bx_sha256_ctx* ctx) {
    ctx->h[0] = 0x6a09e667u;
    ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u;
    ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu;
    ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu;
    ctx->h[7] = 0x5be0cd19u;
    ctx->total_len = 0u;
    ctx->buffer_len = 0u;
}

void bx_sha256_update(struct bx_sha256_ctx* ctx, const void* data_, size_t len) {
    const uint8_t* data = (const uint8_t*)data_;

    ctx->total_len += (uint64_t)len;

    if (ctx->buffer_len > 0u) {
        size_t take = BX_SHA256_BLOCK_SIZE - ctx->buffer_len;
        if (take > len) {
            take = len;
        }

        memcpy(ctx->buffer + ctx->buffer_len, data, take);
        ctx->buffer_len += take;
        data += take;
        len -= take;

        if (ctx->buffer_len == BX_SHA256_BLOCK_SIZE) {
            bx_sha256_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0u;
        }
    }

    while (len >= BX_SHA256_BLOCK_SIZE) {
        bx_sha256_transform(ctx, data);
        data += BX_SHA256_BLOCK_SIZE;
        len -= BX_SHA256_BLOCK_SIZE;
    }

    if (len > 0u) {
        memcpy(ctx->buffer, data, len);
        ctx->buffer_len = len;
    }
}

static void bx_sha256_finalize(struct bx_sha256_ctx* ctx) {
    uint8_t pad[BX_SHA256_BLOCK_SIZE] = {0};
    uint8_t length_be[8];
    uint64_t total_bits = ctx->total_len * 8u;
    size_t pad_len;

    pad[0] = 0x80u;
    for (size_t i = 0; i < 8u; i++) {
        length_be[7u - i] = (uint8_t)(total_bits >> (i * 8u));
    }

    pad_len = (ctx->buffer_len < 56u) ? (56u - ctx->buffer_len) : (64u + 56u - ctx->buffer_len);
    bx_sha256_update(ctx, pad, pad_len);
    bx_sha256_update(ctx, length_be, sizeof(length_be));
}

void bx_sha224_final(struct bx_sha256_ctx* ctx, uint8_t out[BX_SHA224_DIGEST_SIZE]) {
    bx_sha256_finalize(ctx);

    for (size_t i = 0; i < 7u; i++) {
        bx_sha256_store_be32(out + (i * 4u), ctx->h[i]);
    }
}

void bx_sha256_final(struct bx_sha256_ctx* ctx, uint8_t out[BX_SHA256_DIGEST_SIZE]) {
    bx_sha256_finalize(ctx);

    for (size_t i = 0; i < 8u; i++) {
        bx_sha256_store_be32(out + (i * 4u), ctx->h[i]);
    }
}
