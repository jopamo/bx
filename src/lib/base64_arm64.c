#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/arm64_features.h"
#include "lib/base64_internal.h"

#if !defined(BX_BASE64_DISABLE_NEON) && defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_neon.h>

static uint8x16_t bx_base64_arm64_encode_values(uint8x16_t values, uint8x16x4_t alphabet) {
    return vqtbl4q_u8(alphabet, values);
}

size_t bx_base64_arm64_encode_blocks(const uint8_t* input, size_t input_len, char* output) {
    if (!bx_arm64_has_asimd()) {
        return 0u;
    }

    uint8x16x4_t alphabet = {
        .val = {
            vld1q_u8(bx_base64_alphabet + 0u),
            vld1q_u8(bx_base64_alphabet + 16u),
            vld1q_u8(bx_base64_alphabet + 32u),
            vld1q_u8(bx_base64_alphabet + 48u),
        },
    };
    size_t consumed = 0u;
    while (input_len - consumed >= 48u) {
        uint8x16x3_t bytes = vld3q_u8(input + consumed);
        uint8x16x4_t chars;
        uint8x16_t index0 = vshrq_n_u8(bytes.val[0], 2);
        uint8x16_t index1 = vorrq_u8(vshlq_n_u8(vandq_u8(bytes.val[0], vdupq_n_u8(0x03u)), 4), vshrq_n_u8(bytes.val[1], 4));
        uint8x16_t index2 = vorrq_u8(vshlq_n_u8(vandq_u8(bytes.val[1], vdupq_n_u8(0x0fu)), 2), vshrq_n_u8(bytes.val[2], 6));
        uint8x16_t index3 = vandq_u8(bytes.val[2], vdupq_n_u8(0x3fu));

        chars.val[0] = bx_base64_arm64_encode_values(index0, alphabet);
        chars.val[1] = bx_base64_arm64_encode_values(index1, alphabet);
        chars.val[2] = bx_base64_arm64_encode_values(index2, alphabet);
        chars.val[3] = bx_base64_arm64_encode_values(index3, alphabet);
        vst4q_u8((uint8_t*)output + (consumed / 3u) * 4u, chars);
        consumed += 48u;
    }
    return consumed;
}

static bool bx_base64_arm64_decode_values(uint8x16_t chars, uint8x16_t* values) {
    uint8x16_t upper = vandq_u8(vcgeq_u8(chars, vdupq_n_u8('A')), vcleq_u8(chars, vdupq_n_u8('Z')));
    uint8x16_t lower = vandq_u8(vcgeq_u8(chars, vdupq_n_u8('a')), vcleq_u8(chars, vdupq_n_u8('z')));
    uint8x16_t digit = vandq_u8(vcgeq_u8(chars, vdupq_n_u8('0')), vcleq_u8(chars, vdupq_n_u8('9')));
    uint8x16_t plus = vceqq_u8(chars, vdupq_n_u8('+'));
    uint8x16_t slash = vceqq_u8(chars, vdupq_n_u8('/'));
    uint8x16_t valid = vorrq_u8(vorrq_u8(upper, lower), vorrq_u8(vorrq_u8(digit, plus), slash));
    if (vminvq_u8(valid) != UINT8_MAX) {
        return false;
    }

    uint8x16_t result = vandq_u8(upper, vsubq_u8(chars, vdupq_n_u8('A')));
    result = vorrq_u8(result, vandq_u8(lower, vaddq_u8(vsubq_u8(chars, vdupq_n_u8('a')), vdupq_n_u8(26u))));
    result = vorrq_u8(result, vandq_u8(digit, vaddq_u8(vsubq_u8(chars, vdupq_n_u8('0')), vdupq_n_u8(52u))));
    result = vorrq_u8(result, vandq_u8(plus, vdupq_n_u8(62u)));
    result = vorrq_u8(result, vandq_u8(slash, vdupq_n_u8(63u)));
    *values = result;
    return true;
}

bool bx_base64_arm64_decode_64(const unsigned char input[64], uint8_t output[48]) {
    if (!bx_arm64_has_asimd()) {
        return false;
    }

    uint8x16x4_t chars = vld4q_u8(input);
    uint8x16x4_t values;
    if (!bx_base64_arm64_decode_values(chars.val[0], &values.val[0]) || !bx_base64_arm64_decode_values(chars.val[1], &values.val[1]) ||
        !bx_base64_arm64_decode_values(chars.val[2], &values.val[2]) || !bx_base64_arm64_decode_values(chars.val[3], &values.val[3])) {
        return false;
    }

    uint8x16x3_t bytes;
    bytes.val[0] = vorrq_u8(vshlq_n_u8(values.val[0], 2), vshrq_n_u8(values.val[1], 4));
    bytes.val[1] = vorrq_u8(vshlq_n_u8(values.val[1], 4), vshrq_n_u8(values.val[2], 2));
    bytes.val[2] = vorrq_u8(vshlq_n_u8(values.val[2], 6), values.val[3]);
    vst3q_u8(output, bytes);
    return true;
}

#else

size_t bx_base64_arm64_encode_blocks(const uint8_t* input, size_t input_len, char* output) {
    (void)input;
    (void)input_len;
    (void)output;
    return 0u;
}

bool bx_base64_arm64_decode_64(const unsigned char input[64], uint8_t output[48]) {
    (void)input;
    (void)output;
    return false;
}

#endif
