#ifndef BX_SEARCH_LITERAL_ARM64_PROBE_H
#define BX_SEARCH_LITERAL_ARM64_PROBE_H

#include <stdbool.h>

struct bx_literal_arm64_probe {
    bool available;
    unsigned long hwcap;
    unsigned long hwcap2;
};

const struct bx_literal_arm64_probe *bx_literal_arm64_probe_get(void);
bool bx_literal_arm64_probe_has_asimd(const struct bx_literal_arm64_probe *probe);
bool bx_literal_arm64_probe_has_sve(const struct bx_literal_arm64_probe *probe);

#endif
