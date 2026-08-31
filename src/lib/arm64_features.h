#ifndef BX_LIB_ARM64_FEATURES_H
#define BX_LIB_ARM64_FEATURES_H

#include <stdbool.h>

bool bx_arm64_has_asimd(void);
bool bx_arm64_has_crc32(void);
bool bx_arm64_has_sha1(void);
bool bx_arm64_has_sha2(void);

#endif /* BX_LIB_ARM64_FEATURES_H */
