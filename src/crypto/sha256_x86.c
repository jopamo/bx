#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto/sha256_internal.h"

#if !defined(BX_SHA256_DISABLE_HW_ACCEL) && (defined(__i386__) || defined(__x86_64__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>

#define BX_SHA256_X86_TARGET __attribute__((target("sha,ssse3,sse4.1")))

/*
 * Process four rounds.  SHA256RNDS2 consumes the low two message words, so
 * shuffle the upper pair down before the second instruction.
 */
#define BX_SHA256_ROUNDS_4(message, round)                                                                          \
    do {                                                                                                            \
        (message) = _mm_add_epi32((message), _mm_loadu_si128((const __m128i*)&bx_sha256_round_constants[(round)])); \
        state1 = _mm_sha256rnds2_epu32(state1, state0, (message));                                                  \
        (message) = _mm_shuffle_epi32((message), 0x0e);                                                             \
        state0 = _mm_sha256rnds2_epu32(state0, state1, (message));                                                  \
    } while (0)

/*
 * Advance one quarter of the four-register message schedule ring.  The
 * argument order mirrors the SHA extension dataflow: current, previous, next.
 */
#define BX_SHA256_SCHEDULE(current, previous, next)               \
    do {                                                          \
        tmp = _mm_alignr_epi8((current), (previous), 4);          \
        (next) = _mm_add_epi32((next), tmp);                      \
        (next) = _mm_sha256msg2_epu32((next), (current));         \
        (previous) = _mm_sha256msg1_epu32((previous), (current)); \
    } while (0)

/*
 * Intel SHA-extension state layout and message scheduling.  This intrinsic
 * formulation is adapted from the public-domain Intel SHA sample by Jeffrey
 * Walton.  Keeping the multi-block loop here avoids feature checks and state
 * shuffles for every 64-byte block.
 */
static BX_SHA256_X86_TARGET void bx_sha256_x86_sha_ni(uint32_t state[8], const uint8_t* data, size_t block_count) {
    const __m128i byte_swap = _mm_set_epi64x(INT64_C(0x0c0d0e0f08090a0b), INT64_C(0x0405060700010203));
    __m128i state0;
    __m128i state1;
    __m128i message;
    __m128i message0;
    __m128i message1;
    __m128i message2;
    __m128i message3;
    __m128i saved0;
    __m128i saved1;
    __m128i tmp;

    tmp = _mm_loadu_si128((const __m128i*)&state[0]);
    state1 = _mm_loadu_si128((const __m128i*)&state[4]);
    tmp = _mm_shuffle_epi32(tmp, 0xb1);
    state1 = _mm_shuffle_epi32(state1, 0x1b);
    state0 = _mm_alignr_epi8(tmp, state1, 8);
    state1 = _mm_blend_epi16(state1, tmp, 0xf0);

    while (block_count > 0u) {
        saved0 = state0;
        saved1 = state1;

        message0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 0u)), byte_swap);
        message = message0;
        BX_SHA256_ROUNDS_4(message, 0u);

        message1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 16u)), byte_swap);
        message = message1;
        BX_SHA256_ROUNDS_4(message, 4u);
        message0 = _mm_sha256msg1_epu32(message0, message1);

        message2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 32u)), byte_swap);
        message = message2;
        BX_SHA256_ROUNDS_4(message, 8u);
        message1 = _mm_sha256msg1_epu32(message1, message2);

        message3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 48u)), byte_swap);
        message = message3;
        BX_SHA256_ROUNDS_4(message, 12u);
        BX_SHA256_SCHEDULE(message3, message2, message0);

        message = message0;
        BX_SHA256_ROUNDS_4(message, 16u);
        BX_SHA256_SCHEDULE(message0, message3, message1);

        message = message1;
        BX_SHA256_ROUNDS_4(message, 20u);
        BX_SHA256_SCHEDULE(message1, message0, message2);

        message = message2;
        BX_SHA256_ROUNDS_4(message, 24u);
        BX_SHA256_SCHEDULE(message2, message1, message3);

        message = message3;
        BX_SHA256_ROUNDS_4(message, 28u);
        BX_SHA256_SCHEDULE(message3, message2, message0);

        message = message0;
        BX_SHA256_ROUNDS_4(message, 32u);
        BX_SHA256_SCHEDULE(message0, message3, message1);

        message = message1;
        BX_SHA256_ROUNDS_4(message, 36u);
        BX_SHA256_SCHEDULE(message1, message0, message2);

        message = message2;
        BX_SHA256_ROUNDS_4(message, 40u);
        BX_SHA256_SCHEDULE(message2, message1, message3);

        message = message3;
        BX_SHA256_ROUNDS_4(message, 44u);
        BX_SHA256_SCHEDULE(message3, message2, message0);

        message = message0;
        BX_SHA256_ROUNDS_4(message, 48u);
        BX_SHA256_SCHEDULE(message0, message3, message1);

        message = message1;
        BX_SHA256_ROUNDS_4(message, 52u);
        tmp = _mm_alignr_epi8(message1, message0, 4);
        message2 = _mm_sha256msg2_epu32(_mm_add_epi32(message2, tmp), message1);

        message = message2;
        BX_SHA256_ROUNDS_4(message, 56u);
        tmp = _mm_alignr_epi8(message2, message1, 4);
        message3 = _mm_sha256msg2_epu32(_mm_add_epi32(message3, tmp), message2);

        message = message3;
        BX_SHA256_ROUNDS_4(message, 60u);

        state0 = _mm_add_epi32(state0, saved0);
        state1 = _mm_add_epi32(state1, saved1);
        data += BX_SHA256_BLOCK_SIZE;
        block_count--;
    }

    tmp = _mm_shuffle_epi32(state0, 0x1b);
    state1 = _mm_shuffle_epi32(state1, 0xb1);
    state0 = _mm_blend_epi16(tmp, state1, 0xf0);
    state1 = _mm_alignr_epi8(state1, tmp, 8);
    _mm_storeu_si128((__m128i*)&state[0], state0);
    _mm_storeu_si128((__m128i*)&state[4], state1);
}

bool bx_sha256_x86_transform_blocks(struct bx_sha256_ctx* ctx, const uint8_t* data, size_t block_count) {
    if (!__builtin_cpu_supports("sha") || !__builtin_cpu_supports("ssse3") || !__builtin_cpu_supports("sse4.1")) {
        return false;
    }

    bx_sha256_x86_sha_ni(ctx->h, data, block_count);
    return true;
}

#else

bool bx_sha256_x86_transform_blocks(struct bx_sha256_ctx* ctx, const uint8_t* data, size_t block_count) {
    (void)ctx;
    (void)data;
    (void)block_count;
    return false;
}

#endif
