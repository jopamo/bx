#include <stddef.h>
#include <stdint.h>

#include "lib/byte_count.h"
#include "lib/byte_count_internal.h"

uint64_t bx_byte_count(const uint8_t* data, size_t len, uint8_t target) {
    uint64_t count = 0u;
    size_t consumed = bx_byte_count_arm64(data, len, target, &count);
    for (size_t i = consumed; i < len; i++) {
        count += data[i] == target;
    }
    return count;
}
