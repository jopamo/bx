#ifndef BX_LIB_BYTE_TRANSFORM_INTERNAL_H
#define BX_LIB_BYTE_TRANSFORM_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

size_t bx_byte_transform_arm64_map(const uint8_t* input, size_t input_len, uint8_t* output, const uint8_t map[256]);

#endif /* BX_LIB_BYTE_TRANSFORM_INTERNAL_H */
