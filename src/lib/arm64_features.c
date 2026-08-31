#include "lib/arm64_features.h"

#if defined(__linux__) && defined(__aarch64__)
#include <asm/hwcap.h>
#include <sys/auxv.h>

static bool bx_arm64_has_hwcap(unsigned long feature) {
    return (getauxval(AT_HWCAP) & feature) != 0u;
}
#endif

bool bx_arm64_has_asimd(void) {
#if defined(__linux__) && defined(__aarch64__) && defined(HWCAP_ASIMD)
    return bx_arm64_has_hwcap(HWCAP_ASIMD);
#elif defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

bool bx_arm64_has_crc32(void) {
#if defined(__linux__) && defined(__aarch64__) && defined(HWCAP_CRC32)
    return bx_arm64_has_hwcap(HWCAP_CRC32);
#elif defined(__ARM_FEATURE_CRC32)
    return true;
#else
    return false;
#endif
}

bool bx_arm64_has_sha1(void) {
#if defined(__linux__) && defined(__aarch64__) && defined(HWCAP_SHA1)
    return bx_arm64_has_hwcap(HWCAP_SHA1);
#elif defined(__ARM_FEATURE_CRYPTO)
    return true;
#else
    return false;
#endif
}

bool bx_arm64_has_sha2(void) {
#if defined(__linux__) && defined(__aarch64__) && defined(HWCAP_SHA2)
    return bx_arm64_has_hwcap(HWCAP_SHA2);
#elif defined(__ARM_FEATURE_SHA2)
    return true;
#else
    return false;
#endif
}
