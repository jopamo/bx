#ifndef BX_CRYPTO_CRC32B_INTERNAL_H
#define BX_CRYPTO_CRC32B_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool bx_crc32b_arm64_update(uint32_t* crc, const uint8_t* data, size_t len);

#endif /* BX_CRYPTO_CRC32B_INTERNAL_H */
