#ifndef BX_LIB_BASE64_INTERNAL_H
#define BX_LIB_BASE64_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern const uint8_t bx_base64_alphabet[65];

size_t bx_base64_arm64_encode_blocks(const uint8_t* input, size_t input_len, char* output);
bool bx_base64_arm64_decode_64(const unsigned char input[64], uint8_t output[48]);

#endif /* BX_LIB_BASE64_INTERNAL_H */
