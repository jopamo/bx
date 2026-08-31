#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "crypto/crc32b_internal.h"
#include "lib/arm64_features.h"

#if !defined(BX_CRC32B_DISABLE_HW_ACCEL) && defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_acle.h>

#define BX_CRC32B_ARM64_TARGET __attribute__((target("+crc")))

static BX_CRC32B_ARM64_TARGET uint32_t bx_crc32b_arm64_crc(uint32_t crc, const uint8_t* data, size_t len) {
    while (len >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, data, sizeof(word));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        word = __builtin_bswap64(word);
#endif
        crc = __crc32d(crc, word);
        data += sizeof(word);
        len -= sizeof(word);
    }
    while (len > 0u) {
        crc = __crc32b(crc, *data++);
        len--;
    }
    return crc;
}

bool bx_crc32b_arm64_update(uint32_t* crc, const uint8_t* data, size_t len) {
    if (!bx_arm64_has_crc32()) {
        return false;
    }
    *crc = bx_crc32b_arm64_crc(*crc, data, len);
    return true;
}

#else

bool bx_crc32b_arm64_update(uint32_t* crc, const uint8_t* data, size_t len) {
    (void)crc;
    (void)data;
    (void)len;
    return false;
}

#endif
