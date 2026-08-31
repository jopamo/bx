#ifndef BX_CRYPTO_SHA256_INTERNAL_H
#define BX_CRYPTO_SHA256_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto/sha256.h"

extern const uint32_t bx_sha256_round_constants[64];

bool bx_sha256_arm64_transform_blocks(struct bx_sha256_ctx* ctx, const uint8_t* data, size_t block_count);
bool bx_sha256_x86_transform_blocks(struct bx_sha256_ctx* ctx, const uint8_t* data, size_t block_count);

#endif /* BX_CRYPTO_SHA256_INTERNAL_H */
