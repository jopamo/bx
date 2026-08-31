#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto/sha1_internal.h"
#include "lib/arm64_features.h"

#if !defined(BX_SHA1_DISABLE_HW_ACCEL) && defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_neon.h>

#define BX_SHA1_ARM64_TARGET __attribute__((target("+sha2")))

/*
 * ARMv8 SHA-1 state layout and schedule. This intrinsic formulation is
 * adapted from Jeffrey Walton's public-domain SHA-Intrinsics sample.
 */
static BX_SHA1_ARM64_TARGET void bx_sha1_arm64_sha1(uint32_t state[5], const uint8_t* data, size_t block_count) {
    uint32x4_t abcd = vld1q_u32(&state[0]);
    uint32_t e0 = state[4];

    while (block_count > 0u) {
        uint32x4_t saved_abcd = abcd;
        uint32_t saved_e = e0;
        uint32_t e1;
        uint32x4_t tmp0;
        uint32x4_t tmp1;
        uint32x4_t msg0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 0u)));
        uint32x4_t msg1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16u)));
        uint32x4_t msg2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 32u)));
        uint32x4_t msg3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 48u)));

        tmp0 = vaddq_u32(msg0, vdupq_n_u32(0x5a827999u));
        tmp1 = vaddq_u32(msg1, vdupq_n_u32(0x5a827999u));

        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1cq_u32(abcd, e0, tmp0);
        tmp0 = vaddq_u32(msg2, vdupq_n_u32(0x5a827999u));
        msg0 = vsha1su0q_u32(msg0, msg1, msg2);

        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1cq_u32(abcd, e1, tmp1);
        tmp1 = vaddq_u32(msg3, vdupq_n_u32(0x5a827999u));
        msg0 = vsha1su1q_u32(msg0, msg3);
        msg1 = vsha1su0q_u32(msg1, msg2, msg3);

        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1cq_u32(abcd, e0, tmp0);
        tmp0 = vaddq_u32(msg0, vdupq_n_u32(0x5a827999u));
        msg1 = vsha1su1q_u32(msg1, msg0);
        msg2 = vsha1su0q_u32(msg2, msg3, msg0);

        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1cq_u32(abcd, e1, tmp1);
        tmp1 = vaddq_u32(msg1, vdupq_n_u32(0x6ed9eba1u));
        msg2 = vsha1su1q_u32(msg2, msg1);
        msg3 = vsha1su0q_u32(msg3, msg0, msg1);

        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1cq_u32(abcd, e0, tmp0);
        tmp0 = vaddq_u32(msg2, vdupq_n_u32(0x6ed9eba1u));
        msg3 = vsha1su1q_u32(msg3, msg2);
        msg0 = vsha1su0q_u32(msg0, msg1, msg2);

        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1pq_u32(abcd, e1, tmp1);
        tmp1 = vaddq_u32(msg3, vdupq_n_u32(0x6ed9eba1u));
        msg0 = vsha1su1q_u32(msg0, msg3);
        msg1 = vsha1su0q_u32(msg1, msg2, msg3);

        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1pq_u32(abcd, e0, tmp0);
        tmp0 = vaddq_u32(msg0, vdupq_n_u32(0x6ed9eba1u));
        msg1 = vsha1su1q_u32(msg1, msg0);
        msg2 = vsha1su0q_u32(msg2, msg3, msg0);

        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1pq_u32(abcd, e1, tmp1);
        tmp1 = vaddq_u32(msg1, vdupq_n_u32(0x6ed9eba1u));
        msg2 = vsha1su1q_u32(msg2, msg1);
        msg3 = vsha1su0q_u32(msg3, msg0, msg1);

        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1pq_u32(abcd, e0, tmp0);
        tmp0 = vaddq_u32(msg2, vdupq_n_u32(0x8f1bbcdcu));
        msg3 = vsha1su1q_u32(msg3, msg2);
        msg0 = vsha1su0q_u32(msg0, msg1, msg2);

        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1pq_u32(abcd, e1, tmp1);
        tmp1 = vaddq_u32(msg3, vdupq_n_u32(0x8f1bbcdcu));
        msg0 = vsha1su1q_u32(msg0, msg3);
        msg1 = vsha1su0q_u32(msg1, msg2, msg3);

        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1mq_u32(abcd, e0, tmp0);
        tmp0 = vaddq_u32(msg0, vdupq_n_u32(0x8f1bbcdcu));
        msg1 = vsha1su1q_u32(msg1, msg0);
        msg2 = vsha1su0q_u32(msg2, msg3, msg0);

        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1mq_u32(abcd, e1, tmp1);
        tmp1 = vaddq_u32(msg1, vdupq_n_u32(0x8f1bbcdcu));
        msg2 = vsha1su1q_u32(msg2, msg1);
        msg3 = vsha1su0q_u32(msg3, msg0, msg1);

        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1mq_u32(abcd, e0, tmp0);
        tmp0 = vaddq_u32(msg2, vdupq_n_u32(0x8f1bbcdcu));
        msg3 = vsha1su1q_u32(msg3, msg2);
        msg0 = vsha1su0q_u32(msg0, msg1, msg2);

        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1mq_u32(abcd, e1, tmp1);
        tmp1 = vaddq_u32(msg3, vdupq_n_u32(0xca62c1d6u));
        msg0 = vsha1su1q_u32(msg0, msg3);
        msg1 = vsha1su0q_u32(msg1, msg2, msg3);

        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1mq_u32(abcd, e0, tmp0);
        tmp0 = vaddq_u32(msg0, vdupq_n_u32(0xca62c1d6u));
        msg1 = vsha1su1q_u32(msg1, msg0);
        msg2 = vsha1su0q_u32(msg2, msg3, msg0);

        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1pq_u32(abcd, e1, tmp1);
        tmp1 = vaddq_u32(msg1, vdupq_n_u32(0xca62c1d6u));
        msg2 = vsha1su1q_u32(msg2, msg1);
        msg3 = vsha1su0q_u32(msg3, msg0, msg1);

        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1pq_u32(abcd, e0, tmp0);
        tmp0 = vaddq_u32(msg2, vdupq_n_u32(0xca62c1d6u));
        msg3 = vsha1su1q_u32(msg3, msg2);
        msg0 = vsha1su0q_u32(msg0, msg1, msg2);

        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1pq_u32(abcd, e1, tmp1);
        tmp1 = vaddq_u32(msg3, vdupq_n_u32(0xca62c1d6u));
        msg0 = vsha1su1q_u32(msg0, msg3);

        e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1pq_u32(abcd, e0, tmp0);

        e0 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        abcd = vsha1pq_u32(abcd, e1, tmp1);

        abcd = vaddq_u32(abcd, saved_abcd);
        e0 += saved_e;
        data += BX_SHA1_BLOCK_SIZE;
        block_count--;
    }

    vst1q_u32(&state[0], abcd);
    state[4] = e0;
}

bool bx_sha1_arm64_transform_blocks(struct bx_sha1_ctx* ctx, const uint8_t* data, size_t block_count) {
    uint32_t state[5];

    if (!bx_arm64_has_sha1()) {
        return false;
    }

    state[0] = ctx->h0;
    state[1] = ctx->h1;
    state[2] = ctx->h2;
    state[3] = ctx->h3;
    state[4] = ctx->h4;
    bx_sha1_arm64_sha1(state, data, block_count);
    ctx->h0 = state[0];
    ctx->h1 = state[1];
    ctx->h2 = state[2];
    ctx->h3 = state[3];
    ctx->h4 = state[4];
    return true;
}

#else

bool bx_sha1_arm64_transform_blocks(struct bx_sha1_ctx* ctx, const uint8_t* data, size_t block_count) {
    (void)ctx;
    (void)data;
    (void)block_count;
    return false;
}

#endif
