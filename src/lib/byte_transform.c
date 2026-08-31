#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/byte_transform.h"
#include "lib/byte_transform_internal.h"

size_t bx_byte_transform(const uint8_t* input,
                         size_t input_len,
                         uint8_t* output,
                         const uint8_t map[256],
                         const bool delete_set[256],
                         const bool squeeze_set[256],
                         bool delete_enabled,
                         bool squeeze_enabled,
                         struct bx_byte_transform_state* state) {
    if (!delete_enabled && !squeeze_enabled) {
        size_t consumed = bx_byte_transform_arm64_map(input, input_len, output, map);
        for (size_t i = consumed; i < input_len; i++) {
            output[i] = map[input[i]];
        }
        if (input_len > 0u) {
            state->previous_output = output[input_len - 1u];
        }
        return input_len;
    }

    size_t output_len = 0u;
    int previous = state->previous_output;
    for (size_t i = 0u; i < input_len; i++) {
        uint8_t byte = input[i];
        if (delete_enabled && delete_set[byte]) {
            continue;
        }

        uint8_t translated = map[byte];
        if (squeeze_enabled && previous == (int)translated && squeeze_set[translated]) {
            continue;
        }
        output[output_len++] = translated;
        previous = translated;
    }
    state->previous_output = previous;
    return output_len;
}
