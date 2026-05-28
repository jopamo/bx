#ifndef BX_SEARCH_LITERAL_X86_PROBE_H
#define BX_SEARCH_LITERAL_X86_PROBE_H

#include <stdbool.h>
#include <stdint.h>

struct bx_literal_x86_probe {
    bool available;
    uint32_t max_basic_leaf;
    uint32_t leaf1_ecx;
    uint32_t leaf1_edx;
    uint32_t leaf7_ebx;
    uint64_t xcr0;
};

const struct bx_literal_x86_probe *bx_literal_x86_probe_get(void);
bool bx_literal_x86_probe_has_avx2(const struct bx_literal_x86_probe *probe);
bool bx_literal_x86_probe_has_avx512bw(const struct bx_literal_x86_probe *probe);

#endif
