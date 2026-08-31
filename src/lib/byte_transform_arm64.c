#include <stddef.h>
#include <stdint.h>

#include "lib/arm64_features.h"
#include "lib/byte_transform_internal.h"

#if !defined(BX_BYTE_TRANSFORM_DISABLE_NEON) && defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_neon.h>

size_t bx_byte_transform_arm64_map(const uint8_t* input, size_t input_len, uint8_t* output, const uint8_t map[256]) {
    if (!bx_arm64_has_asimd()) {
        return 0u;
    }

    const uint8x16x4_t map0 = vld1q_u8_x4(map);
    const uint8x16x4_t map1 = vld1q_u8_x4(map + 64u);
    const uint8x16x4_t map2 = vld1q_u8_x4(map + 128u);
    const uint8x16x4_t map3 = vld1q_u8_x4(map + 192u);
    const uint8x16_t low_mask = vdupq_n_u8(0x3fu);
    size_t consumed = 0u;

    while (input_len - consumed >= 16u) {
        uint8x16_t bytes = vld1q_u8(input + consumed);
        uint8x16_t low = vandq_u8(bytes, low_mask);
        uint8x16_t high = vshrq_n_u8(bytes, 6);
        uint8x16_t mapped = vqtbl4q_u8(map0, low);
        mapped = vbslq_u8(vceqq_u8(high, vdupq_n_u8(1u)), vqtbl4q_u8(map1, low), mapped);
        mapped = vbslq_u8(vceqq_u8(high, vdupq_n_u8(2u)), vqtbl4q_u8(map2, low), mapped);
        mapped = vbslq_u8(vceqq_u8(high, vdupq_n_u8(3u)), vqtbl4q_u8(map3, low), mapped);
        vst1q_u8(output + consumed, mapped);
        consumed += 16u;
    }
    return consumed;
}

#else

size_t bx_byte_transform_arm64_map(const uint8_t* input, size_t input_len, uint8_t* output, const uint8_t map[256]) {
    (void)input;
    (void)input_len;
    (void)output;
    (void)map;
    return 0u;
}

#endif
