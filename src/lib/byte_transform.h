#ifndef BX_LIB_BYTE_TRANSFORM_H
#define BX_LIB_BYTE_TRANSFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct bx_byte_transform_state {
    int previous_output;
};

size_t bx_byte_transform(const uint8_t* input,
                         size_t input_len,
                         uint8_t* output,
                         const uint8_t map[256],
                         const bool delete_set[256],
                         const bool squeeze_set[256],
                         bool delete_enabled,
                         bool squeeze_enabled,
                         struct bx_byte_transform_state* state);

#endif /* BX_LIB_BYTE_TRANSFORM_H */
