#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common/sha512.h"

static uint64_t bx_sha512_rotr64(uint64_t value, unsigned shift) {
    return (value >> shift) | (value << (64u - shift));
}

static uint64_t bx_sha512_load_be64(const uint8_t* bytes) {
    return ((uint64_t)bytes[0] << 56u) | ((uint64_t)bytes[1] << 48u) | ((uint64_t)bytes[2] << 40u) | ((uint64_t)bytes[3] << 32u) | ((uint64_t)bytes[4] << 24u) | ((uint64_t)bytes[5] << 16u) |
           ((uint64_t)bytes[6] << 8u) | (uint64_t)bytes[7];
}

static void bx_sha512_store_be64(uint8_t* out, uint64_t value) {
    out[0] = (uint8_t)(value >> 56u);
    out[1] = (uint8_t)(value >> 48u);
    out[2] = (uint8_t)(value >> 40u);
    out[3] = (uint8_t)(value >> 32u);
    out[4] = (uint8_t)(value >> 24u);
    out[5] = (uint8_t)(value >> 16u);
    out[6] = (uint8_t)(value >> 8u);
    out[7] = (uint8_t)value;
}

static uint64_t bx_sha512_ch(uint64_t x, uint64_t y, uint64_t z) {
    return (x & y) ^ ((~x) & z);
}

static uint64_t bx_sha512_maj(uint64_t x, uint64_t y, uint64_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint64_t bx_sha512_big_sigma0(uint64_t x) {
    return bx_sha512_rotr64(x, 28u) ^ bx_sha512_rotr64(x, 34u) ^ bx_sha512_rotr64(x, 39u);
}

static uint64_t bx_sha512_big_sigma1(uint64_t x) {
    return bx_sha512_rotr64(x, 14u) ^ bx_sha512_rotr64(x, 18u) ^ bx_sha512_rotr64(x, 41u);
}

static uint64_t bx_sha512_small_sigma0(uint64_t x) {
    return bx_sha512_rotr64(x, 1u) ^ bx_sha512_rotr64(x, 8u) ^ (x >> 7u);
}

static uint64_t bx_sha512_small_sigma1(uint64_t x) {
    return bx_sha512_rotr64(x, 19u) ^ bx_sha512_rotr64(x, 61u) ^ (x >> 6u);
}

static void bx_sha512_transform(struct bx_sha512_ctx* ctx, const uint8_t block[BX_SHA512_BLOCK_SIZE]) {
    static const uint64_t k[80] = {
        UINT64_C(0x428a2f98d728ae22), UINT64_C(0x7137449123ef65cd), UINT64_C(0xb5c0fbcfec4d3b2f), UINT64_C(0xe9b5dba58189dbbc), UINT64_C(0x3956c25bf348b538), UINT64_C(0x59f111f1b605d019),
        UINT64_C(0x923f82a4af194f9b), UINT64_C(0xab1c5ed5da6d8118), UINT64_C(0xd807aa98a3030242), UINT64_C(0x12835b0145706fbe), UINT64_C(0x243185be4ee4b28c), UINT64_C(0x550c7dc3d5ffb4e2),
        UINT64_C(0x72be5d74f27b896f), UINT64_C(0x80deb1fe3b1696b1), UINT64_C(0x9bdc06a725c71235), UINT64_C(0xc19bf174cf692694), UINT64_C(0xe49b69c19ef14ad2), UINT64_C(0xefbe4786384f25e3),
        UINT64_C(0x0fc19dc68b8cd5b5), UINT64_C(0x240ca1cc77ac9c65), UINT64_C(0x2de92c6f592b0275), UINT64_C(0x4a7484aa6ea6e483), UINT64_C(0x5cb0a9dcbd41fbd4), UINT64_C(0x76f988da831153b5),
        UINT64_C(0x983e5152ee66dfab), UINT64_C(0xa831c66d2db43210), UINT64_C(0xb00327c898fb213f), UINT64_C(0xbf597fc7beef0ee4), UINT64_C(0xc6e00bf33da88fc2), UINT64_C(0xd5a79147930aa725),
        UINT64_C(0x06ca6351e003826f), UINT64_C(0x142929670a0e6e70), UINT64_C(0x27b70a8546d22ffc), UINT64_C(0x2e1b21385c26c926), UINT64_C(0x4d2c6dfc5ac42aed), UINT64_C(0x53380d139d95b3df),
        UINT64_C(0x650a73548baf63de), UINT64_C(0x766a0abb3c77b2a8), UINT64_C(0x81c2c92e47edaee6), UINT64_C(0x92722c851482353b), UINT64_C(0xa2bfe8a14cf10364), UINT64_C(0xa81a664bbc423001),
        UINT64_C(0xc24b8b70d0f89791), UINT64_C(0xc76c51a30654be30), UINT64_C(0xd192e819d6ef5218), UINT64_C(0xd69906245565a910), UINT64_C(0xf40e35855771202a), UINT64_C(0x106aa07032bbd1b8),
        UINT64_C(0x19a4c116b8d2d0c8), UINT64_C(0x1e376c085141ab53), UINT64_C(0x2748774cdf8eeb99), UINT64_C(0x34b0bcb5e19b48a8), UINT64_C(0x391c0cb3c5c95a63), UINT64_C(0x4ed8aa4ae3418acb),
        UINT64_C(0x5b9cca4f7763e373), UINT64_C(0x682e6ff3d6b2b8a3), UINT64_C(0x748f82ee5defb2fc), UINT64_C(0x78a5636f43172f60), UINT64_C(0x84c87814a1f0ab72), UINT64_C(0x8cc702081a6439ec),
        UINT64_C(0x90befffa23631e28), UINT64_C(0xa4506cebde82bde9), UINT64_C(0xbef9a3f7b2c67915), UINT64_C(0xc67178f2e372532b), UINT64_C(0xca273eceea26619c), UINT64_C(0xd186b8c721c0c207),
        UINT64_C(0xeada7dd6cde0eb1e), UINT64_C(0xf57d4f7fee6ed178), UINT64_C(0x06f067aa72176fba), UINT64_C(0x0a637dc5a2c898a6), UINT64_C(0x113f9804bef90dae), UINT64_C(0x1b710b35131c471b),
        UINT64_C(0x28db77f523047d84), UINT64_C(0x32caab7b40c72493), UINT64_C(0x3c9ebe0a15c9bebc), UINT64_C(0x431d67c49c100d4c), UINT64_C(0x4cc5d4becb3e42b6), UINT64_C(0x597f299cfc657e2a),
        UINT64_C(0x5fcb6fab3ad6faec), UINT64_C(0x6c44198c4a475817),
    };

    uint64_t w[80];
    uint64_t a = ctx->h[0];
    uint64_t b = ctx->h[1];
    uint64_t c = ctx->h[2];
    uint64_t d = ctx->h[3];
    uint64_t e = ctx->h[4];
    uint64_t f = ctx->h[5];
    uint64_t g = ctx->h[6];
    uint64_t h = ctx->h[7];

    for (size_t i = 0; i < 16u; i++) {
        w[i] = bx_sha512_load_be64(block + (i * 8u));
    }

    for (size_t i = 16u; i < 80u; i++) {
        w[i] = bx_sha512_small_sigma1(w[i - 2u]) + w[i - 7u] + bx_sha512_small_sigma0(w[i - 15u]) + w[i - 16u];
    }

    for (size_t i = 0; i < 80u; i++) {
        uint64_t t1 = h + bx_sha512_big_sigma1(e) + bx_sha512_ch(e, f, g) + k[i] + w[i];
        uint64_t t2 = bx_sha512_big_sigma0(a) + bx_sha512_maj(a, b, c);

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

void bx_sha384_init(struct bx_sha512_ctx* ctx) {
    ctx->h[0] = UINT64_C(0xcbbb9d5dc1059ed8);
    ctx->h[1] = UINT64_C(0x629a292a367cd507);
    ctx->h[2] = UINT64_C(0x9159015a3070dd17);
    ctx->h[3] = UINT64_C(0x152fecd8f70e5939);
    ctx->h[4] = UINT64_C(0x67332667ffc00b31);
    ctx->h[5] = UINT64_C(0x8eb44a8768581511);
    ctx->h[6] = UINT64_C(0xdb0c2e0d64f98fa7);
    ctx->h[7] = UINT64_C(0x47b5481dbefa4fa4);
    ctx->total_len_hi = 0u;
    ctx->total_len_lo = 0u;
    ctx->buffer_len = 0u;
}

void bx_sha512_init(struct bx_sha512_ctx* ctx) {
    ctx->h[0] = UINT64_C(0x6a09e667f3bcc908);
    ctx->h[1] = UINT64_C(0xbb67ae8584caa73b);
    ctx->h[2] = UINT64_C(0x3c6ef372fe94f82b);
    ctx->h[3] = UINT64_C(0xa54ff53a5f1d36f1);
    ctx->h[4] = UINT64_C(0x510e527fade682d1);
    ctx->h[5] = UINT64_C(0x9b05688c2b3e6c1f);
    ctx->h[6] = UINT64_C(0x1f83d9abfb41bd6b);
    ctx->h[7] = UINT64_C(0x5be0cd19137e2179);
    ctx->total_len_hi = 0u;
    ctx->total_len_lo = 0u;
    ctx->buffer_len = 0u;
}

static void bx_sha512_add_len(struct bx_sha512_ctx* ctx, size_t len) {
    uint64_t prev_lo = ctx->total_len_lo;
    ctx->total_len_lo += (uint64_t)len;
    if (ctx->total_len_lo < prev_lo) {
        ctx->total_len_hi++;
    }
}

void bx_sha512_update(struct bx_sha512_ctx* ctx, const void* data_, size_t len) {
    const uint8_t* data = (const uint8_t*)data_;

    bx_sha512_add_len(ctx, len);

    if (ctx->buffer_len > 0u) {
        size_t take = BX_SHA512_BLOCK_SIZE - ctx->buffer_len;
        if (take > len) {
            take = len;
        }

        memcpy(ctx->buffer + ctx->buffer_len, data, take);
        ctx->buffer_len += take;
        data += take;
        len -= take;

        if (ctx->buffer_len == BX_SHA512_BLOCK_SIZE) {
            bx_sha512_transform(ctx, ctx->buffer);
            ctx->buffer_len = 0u;
        }
    }

    while (len >= BX_SHA512_BLOCK_SIZE) {
        bx_sha512_transform(ctx, data);
        data += BX_SHA512_BLOCK_SIZE;
        len -= BX_SHA512_BLOCK_SIZE;
    }

    if (len > 0u) {
        memcpy(ctx->buffer, data, len);
        ctx->buffer_len = len;
    }
}

static void bx_sha512_finalize(struct bx_sha512_ctx* ctx) {
    uint8_t pad[BX_SHA512_BLOCK_SIZE] = {0};
    uint8_t length_be[16];
    size_t pad_len;

    uint64_t total_bits_hi = (ctx->total_len_hi << 3u) | (ctx->total_len_lo >> 61u);
    uint64_t total_bits_lo = ctx->total_len_lo << 3u;

    bx_sha512_store_be64(length_be, total_bits_hi);
    bx_sha512_store_be64(length_be + 8u, total_bits_lo);

    pad[0] = 0x80u;
    pad_len = (ctx->buffer_len < 112u) ? (112u - ctx->buffer_len) : (BX_SHA512_BLOCK_SIZE + 112u - ctx->buffer_len);
    bx_sha512_update(ctx, pad, pad_len);
    bx_sha512_update(ctx, length_be, sizeof(length_be));
}

void bx_sha384_final(struct bx_sha512_ctx* ctx, uint8_t out[BX_SHA384_DIGEST_SIZE]) {
    bx_sha512_finalize(ctx);

    for (size_t i = 0; i < 6u; i++) {
        bx_sha512_store_be64(out + (i * 8u), ctx->h[i]);
    }
}

void bx_sha512_final(struct bx_sha512_ctx* ctx, uint8_t out[BX_SHA512_DIGEST_SIZE]) {
    bx_sha512_finalize(ctx);

    for (size_t i = 0; i < 8u; i++) {
        bx_sha512_store_be64(out + (i * 8u), ctx->h[i]);
    }
}
