#include <stddef.h>
#include <stdint.h>

#include "lib/arm64_features.h"
#include "lib/byte_count_internal.h"

#if !defined(BX_BYTE_COUNT_DISABLE_NEON) && defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_neon.h>

size_t bx_byte_count_arm64(const uint8_t* data, size_t len, uint8_t target, uint64_t* count) {
    if (!bx_arm64_has_asimd()) {
        return 0u;
    }

    const uint8x16_t needle = vdupq_n_u8(target);
    size_t consumed = 0u;
    uint64_t total = 0u;
    while (len - consumed >= 64u) {
        uint8x16_t matches0 = vshrq_n_u8(vceqq_u8(vld1q_u8(data + consumed), needle), 7);
        uint8x16_t matches1 = vshrq_n_u8(vceqq_u8(vld1q_u8(data + consumed + 16u), needle), 7);
        uint8x16_t matches2 = vshrq_n_u8(vceqq_u8(vld1q_u8(data + consumed + 32u), needle), 7);
        uint8x16_t matches3 = vshrq_n_u8(vceqq_u8(vld1q_u8(data + consumed + 48u), needle), 7);
        uint8x16_t block_sum = vaddq_u8(vaddq_u8(matches0, matches1), vaddq_u8(matches2, matches3));
        total += vaddlvq_u8(block_sum);
        consumed += 64u;
    }
    *count = total;
    return consumed;
}

#else

size_t bx_byte_count_arm64(const uint8_t* data, size_t len, uint8_t target, uint64_t* count) {
    (void)data;
    (void)len;
    (void)target;
    *count = 0u;
    return 0u;
}

#endif
