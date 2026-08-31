#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto/sha256_internal.h"
#include "lib/arm64_features.h"

#if !defined(BX_SHA256_DISABLE_HW_ACCEL) && defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_neon.h>

#define BX_SHA256_ARM64_TARGET __attribute__((target("+sha2")))

#define BX_SHA256_ROUNDS_4(message, round)                                                                  \
    do {                                                                                                    \
        uint32x4_t bx_sha256_state0 = state0;                                                               \
        uint32x4_t bx_sha256_message =                                                                      \
            vaddq_u32((message), vld1q_u32(&bx_sha256_round_constants[(round)]));                           \
        state0 = vsha256hq_u32(state0, state1, bx_sha256_message);                                          \
        state1 = vsha256h2q_u32(state1, bx_sha256_state0, bx_sha256_message);                               \
    } while (0)

#define BX_SHA256_SCHEDULE(current, next, next2, next3)            \
    do {                                                           \
        (current) = vsha256su0q_u32((current), (next));            \
        (current) = vsha256su1q_u32((current), (next2), (next3));  \
    } while (0)

static BX_SHA256_ARM64_TARGET void bx_sha256_arm64_sha2(uint32_t state[8], const uint8_t* data, size_t block_count) {
    uint32x4_t state0 = vld1q_u32(&state[0]);
    uint32x4_t state1 = vld1q_u32(&state[4]);

    while (block_count > 0u) {
        uint32x4_t saved0 = state0;
        uint32x4_t saved1 = state1;
        uint32x4_t message0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 0u)));
        uint32x4_t message1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16u)));
        uint32x4_t message2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 32u)));
        uint32x4_t message3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 48u)));

        BX_SHA256_ROUNDS_4(message0, 0u);
        BX_SHA256_ROUNDS_4(message1, 4u);
        BX_SHA256_ROUNDS_4(message2, 8u);
        BX_SHA256_ROUNDS_4(message3, 12u);

        BX_SHA256_SCHEDULE(message0, message1, message2, message3);
        BX_SHA256_ROUNDS_4(message0, 16u);
        BX_SHA256_SCHEDULE(message1, message2, message3, message0);
        BX_SHA256_ROUNDS_4(message1, 20u);
        BX_SHA256_SCHEDULE(message2, message3, message0, message1);
        BX_SHA256_ROUNDS_4(message2, 24u);
        BX_SHA256_SCHEDULE(message3, message0, message1, message2);
        BX_SHA256_ROUNDS_4(message3, 28u);

        BX_SHA256_SCHEDULE(message0, message1, message2, message3);
        BX_SHA256_ROUNDS_4(message0, 32u);
        BX_SHA256_SCHEDULE(message1, message2, message3, message0);
        BX_SHA256_ROUNDS_4(message1, 36u);
        BX_SHA256_SCHEDULE(message2, message3, message0, message1);
        BX_SHA256_ROUNDS_4(message2, 40u);
        BX_SHA256_SCHEDULE(message3, message0, message1, message2);
        BX_SHA256_ROUNDS_4(message3, 44u);

        BX_SHA256_SCHEDULE(message0, message1, message2, message3);
        BX_SHA256_ROUNDS_4(message0, 48u);
        BX_SHA256_SCHEDULE(message1, message2, message3, message0);
        BX_SHA256_ROUNDS_4(message1, 52u);
        BX_SHA256_SCHEDULE(message2, message3, message0, message1);
        BX_SHA256_ROUNDS_4(message2, 56u);
        BX_SHA256_SCHEDULE(message3, message0, message1, message2);
        BX_SHA256_ROUNDS_4(message3, 60u);

        state0 = vaddq_u32(state0, saved0);
        state1 = vaddq_u32(state1, saved1);
        data += BX_SHA256_BLOCK_SIZE;
        block_count--;
    }

    vst1q_u32(&state[0], state0);
    vst1q_u32(&state[4], state1);
}

bool bx_sha256_arm64_transform_blocks(struct bx_sha256_ctx* ctx, const uint8_t* data, size_t block_count) {
    if (!bx_arm64_has_sha2()) {
        return false;
    }

    bx_sha256_arm64_sha2(ctx->h, data, block_count);
    return true;
}

#else

bool bx_sha256_arm64_transform_blocks(struct bx_sha256_ctx* ctx, const uint8_t* data, size_t block_count) {
    (void)ctx;
    (void)data;
    (void)block_count;
    return false;
}

#endif
