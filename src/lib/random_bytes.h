#ifndef BX_COMMON_RANDOM_BYTES_H
#define BX_COMMON_RANDOM_BYTES_H

#include <stdbool.h>
#include <stddef.h>

bool bx_random_bytes(void* buffer, size_t length);
bool bx_random_bytes_nonblocking(void* buffer, size_t length);

#endif /* BX_COMMON_RANDOM_BYTES_H */
