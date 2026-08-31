#ifndef BX_LIB_BYTE_COUNT_H
#define BX_LIB_BYTE_COUNT_H

#include <stddef.h>
#include <stdint.h>

uint64_t bx_byte_count(const uint8_t* data, size_t len, uint8_t target);

#endif /* BX_LIB_BYTE_COUNT_H */
