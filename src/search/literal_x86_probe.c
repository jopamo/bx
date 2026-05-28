#include "literal_x86_probe.h"

#include <pthread.h>

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

static pthread_once_t bx_literal_x86_probe_once = PTHREAD_ONCE_INIT;
static struct bx_literal_x86_probe bx_literal_x86_probe_state = {0};

#if defined(__i386__) || defined(__x86_64__)
static uint64_t bx_literal_x86_probe_read_xcr0(void) {
    uint32_t eax = 0u;
    uint32_t edx = 0u;

    __asm__ volatile(".byte 0x0f, 0x01, 0xd0"
                     : "=a"(eax), "=d"(edx)
                     : "c"(0u));
    return ((uint64_t)edx << 32) | (uint64_t)eax;
}
#endif

static void bx_literal_x86_probe_init(void) {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int eax = 0u;
    unsigned int ebx = 0u;
    unsigned int ecx = 0u;
    unsigned int edx = 0u;
    unsigned int max_basic_leaf = __get_cpuid_max(0u, NULL);

    bx_literal_x86_probe_state.available = true;
    bx_literal_x86_probe_state.max_basic_leaf = max_basic_leaf;

    if (max_basic_leaf >= 1u && __get_cpuid(1u, &eax, &ebx, &ecx, &edx)) {
        bx_literal_x86_probe_state.leaf1_ecx = ecx;
        bx_literal_x86_probe_state.leaf1_edx = edx;
        if ((ecx & (1u << 26)) != 0u && (ecx & (1u << 27)) != 0u)
            bx_literal_x86_probe_state.xcr0 = bx_literal_x86_probe_read_xcr0();
    }

    if (max_basic_leaf >= 7u) {
        __cpuid_count(7u, 0u, eax, ebx, ecx, edx);
        bx_literal_x86_probe_state.leaf7_ebx = ebx;
    }
#endif
}

const struct bx_literal_x86_probe *bx_literal_x86_probe_get(void) {
    pthread_once(&bx_literal_x86_probe_once, bx_literal_x86_probe_init);
    return &bx_literal_x86_probe_state;
}

bool bx_literal_x86_probe_has_avx2(const struct bx_literal_x86_probe *probe) {
#if defined(__i386__) || defined(__x86_64__)
    if (!probe || !probe->available || probe->max_basic_leaf < 7u)
        return false;
    if ((probe->leaf1_ecx & (1u << 26)) == 0u)
        return false;
    if ((probe->leaf1_ecx & (1u << 27)) == 0u)
        return false;
    if ((probe->leaf1_ecx & (1u << 28)) == 0u)
        return false;
    if ((probe->xcr0 & 0x6u) != 0x6u)
        return false;
    return (probe->leaf7_ebx & (1u << 5)) != 0u;
#else
    (void)probe;
    return false;
#endif
}

bool bx_literal_x86_probe_has_avx512bw(const struct bx_literal_x86_probe *probe) {
#if defined(__i386__) || defined(__x86_64__)
    if (!probe || !probe->available || probe->max_basic_leaf < 7u)
        return false;
    if ((probe->leaf1_ecx & (1u << 26)) == 0u)
        return false;
    if ((probe->leaf1_ecx & (1u << 27)) == 0u)
        return false;
    if ((probe->leaf1_ecx & (1u << 28)) == 0u)
        return false;
    if ((probe->xcr0 & 0xe6u) != 0xe6u)
        return false;
    if ((probe->leaf7_ebx & (1u << 16)) == 0u)
        return false;
    return (probe->leaf7_ebx & (1u << 30)) != 0u;
#else
    (void)probe;
    return false;
#endif
}
