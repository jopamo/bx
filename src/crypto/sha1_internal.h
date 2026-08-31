#ifndef BX_CRYPTO_SHA1_INTERNAL_H
#define BX_CRYPTO_SHA1_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto/sha1.h"

bool bx_sha1_arm64_transform_blocks(struct bx_sha1_ctx* ctx, const uint8_t* data, size_t block_count);

#endif /* BX_CRYPTO_SHA1_INTERNAL_H */
