#ifndef BX_LIB_BYTE_COUNT_INTERNAL_H
#define BX_LIB_BYTE_COUNT_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

size_t bx_byte_count_arm64(const uint8_t* data, size_t len, uint8_t target, uint64_t* count);

#endif /* BX_LIB_BYTE_COUNT_INTERNAL_H */
