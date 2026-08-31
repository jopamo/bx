#ifndef BX_LIB_BASE64_H
#define BX_LIB_BASE64_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t bx_base64_encoded_size(size_t input_len);
size_t bx_base64_encode(const uint8_t* input, size_t input_len, char* output);
size_t bx_base64_encode_complete(const uint8_t* input, size_t input_len, char* output);
size_t bx_base64_decode_blocks(const unsigned char* input, size_t input_len, uint8_t* output);
bool bx_base64_decode_exact(const char* input, size_t input_len, uint8_t* output, size_t* output_len);
int bx_base64_decode_value(unsigned char ch);

#endif /* BX_LIB_BASE64_H */
