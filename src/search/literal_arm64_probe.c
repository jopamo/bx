#include "literal_arm64_probe.h"

#include <pthread.h>

#if defined(__linux__) && defined(__aarch64__)
#include <errno.h>
#include <sys/auxv.h>

#if defined(__has_include)
#if __has_include(<asm/hwcap.h>)
#include <asm/hwcap.h>
#endif
#endif

#ifndef AT_HWCAP2
#define AT_HWCAP2 26
#endif

#ifndef HWCAP_ASIMD
#define HWCAP_ASIMD (1UL << 1)
#endif

#ifndef HWCAP_SVE
#define HWCAP_SVE (1UL << 22)
#endif
#endif

static pthread_once_t bx_literal_arm64_probe_once = PTHREAD_ONCE_INIT;
static struct bx_literal_arm64_probe bx_literal_arm64_probe_state = {0};

static void bx_literal_arm64_probe_init(void) {
#if defined(__linux__) && defined(__aarch64__)
    errno = 0;
    bx_literal_arm64_probe_state.hwcap = getauxval(AT_HWCAP);
    if (errno != 0) {
        bx_literal_arm64_probe_state.hwcap = 0ul;
        return;
    }

    bx_literal_arm64_probe_state.available = true;
    errno = 0;
    bx_literal_arm64_probe_state.hwcap2 = getauxval(AT_HWCAP2);
    if (errno != 0)
        bx_literal_arm64_probe_state.hwcap2 = 0ul;
#endif
}

const struct bx_literal_arm64_probe *bx_literal_arm64_probe_get(void) {
    pthread_once(&bx_literal_arm64_probe_once, bx_literal_arm64_probe_init);
    return &bx_literal_arm64_probe_state;
}

bool bx_literal_arm64_probe_has_asimd(const struct bx_literal_arm64_probe *probe) {
#if defined(__linux__) && defined(__aarch64__)
    return probe && probe->available && (probe->hwcap & HWCAP_ASIMD) != 0ul;
#else
    (void)probe;
    return false;
#endif
}

bool bx_literal_arm64_probe_has_sve(const struct bx_literal_arm64_probe *probe) {
#if defined(__linux__) && defined(__aarch64__)
    return probe && probe->available && (probe->hwcap & HWCAP_SVE) != 0ul;
#else
    (void)probe;
    return false;
#endif
}
