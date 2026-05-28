#define _GNU_SOURCE
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#if BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS
#include <arm_sve.h>
#endif
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#endif
#include "dev_counters.h"
#include "literal.h"
#include "literal_arm64_probe.h"
#include "literal_scan.h"
#include "literal_plan.h"
#include "literal_x86_probe.h"
#include "rg_text.h"

typedef int (*bx_literal_find_fn)(const struct bx_literal_matcher *m,
                                  const unsigned char *buf,
                                  size_t len,
                                  size_t start,
                                  struct bx_match *out);

typedef int (*bx_literal_case_sensitive_find_fn)(const struct bx_literal_matcher *m,
                                                 const unsigned char *buf,
                                                 size_t len,
                                                 size_t start,
                                                 struct bx_match *out);

#if (defined(__i386__) || defined(__x86_64__)) && (defined(__GNUC__) || defined(__clang__))
#define BX_LITERAL_HAVE_AVX2_TARGET 1
#define BX_LITERAL_AVX2_TARGET __attribute__((target("avx2")))
#else
#define BX_LITERAL_HAVE_AVX2_TARGET 0
#define BX_LITERAL_AVX2_TARGET
#endif
#if defined(__aarch64__)
#if defined(__clang__)
#define BX_LITERAL_HAVE_ARM64_VMAXVQ_U8 1
#elif defined(__GNUC__) && __GNUC__ >= 8
#define BX_LITERAL_HAVE_ARM64_VMAXVQ_U8 1
#else
#define BX_LITERAL_HAVE_ARM64_VMAXVQ_U8 0
#endif
#endif

static void bx_literal_select_case_sensitive_backend(struct bx_literal_matcher *m);
static bool bx_literal_match_at_anchor(const struct bx_literal_matcher *m,
                                       const unsigned char *buf,
                                       size_t pos);
static int bx_literal_find_empty_compiled(const struct bx_literal_matcher *m,
                                          const unsigned char *buf,
                                          size_t len,
                                          size_t start,
                                          struct bx_match *out);
static int bx_literal_find_byte_compiled(const struct bx_literal_matcher *m,
                                         const unsigned char *buf,
                                         size_t len,
                                         size_t start,
                                         struct bx_match *out);
static int bx_literal_find_pair_compiled(const struct bx_literal_matcher *m,
                                         const unsigned char *buf,
                                         size_t len,
                                         size_t start,
                                         struct bx_match *out);
static int bx_literal_find_short_compiled(const struct bx_literal_matcher *m,
                                          const unsigned char *buf,
                                          size_t len,
                                          size_t start,
                                          struct bx_match *out);
static int bx_literal_find_non_empty(const struct bx_literal_matcher *m,
                                     const unsigned char *buf,
                                     size_t len,
                                     size_t start,
                                     struct bx_match *out);
static int bx_literal_find_case_sensitive_byte(const struct bx_literal_matcher *m,
                                               const unsigned char *buf,
                                               size_t len,
                                               size_t start,
                                               struct bx_match *out);
static int bx_literal_find_case_sensitive_pair(const struct bx_literal_matcher *m,
                                               const unsigned char *buf,
                                               size_t len,
                                               size_t start,
                                               struct bx_match *out);
static int bx_literal_find_case_sensitive_short(const struct bx_literal_matcher *m,
                                                const unsigned char *buf,
                                                size_t len,
                                                size_t start,
                                                struct bx_match *out);
#if defined(__aarch64__)
static int bx_literal_find_case_sensitive_arm64_neon(const struct bx_literal_matcher *m,
                                                     const unsigned char *buf,
                                                     size_t len,
                                                     size_t start,
                                                     struct bx_match *out);
#if BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS
static int bx_literal_find_case_sensitive_arm64_sve(const struct bx_literal_matcher *m,
                                                    const unsigned char *buf,
                                                    size_t len,
                                                    size_t start,
                                                    struct bx_match *out);
static enum bx_lit_result bx_literal_arm64_sve_confirm_candidate_lanes_scalar(
    const unsigned char *candidate_lanes,
    size_t active_lanes,
    const unsigned char *buf,
    size_t search_off,
    size_t pair_offset,
    const unsigned char *needle,
    size_t needle_len,
    size_t *match_off);
#endif
static enum bx_lit_result bx_literal_arm64_neon_confirm_candidate_lanes_scalar(
    uint16_t lane_mask,
    const unsigned char *buf,
    size_t search_off,
    size_t search_limit,
    size_t pair_offset,
    const unsigned char *needle,
    size_t needle_len,
    size_t *match_off);
static enum bx_lit_result bx_literal_arm64_neon_confirm_candidate_lanes_bitmask(
    uint16_t lane_mask,
    const unsigned char *buf,
    size_t search_off,
    size_t search_limit,
    size_t pair_offset,
    const unsigned char *needle,
    size_t needle_len,
    size_t *match_off);
#endif
static int bx_literal_find_case_sensitive_avx2(const struct bx_literal_matcher *m,
                                               const unsigned char *buf,
                                               size_t len,
                                               size_t start,
                                               struct bx_match *out);
static int bx_literal_find_case_sensitive_compiled(const struct bx_literal_matcher *m,
                                                   const unsigned char *buf,
                                                   size_t len,
                                                   size_t start,
                                                   struct bx_match *out);
static bool bx_literal_can_use_sse2(const struct bx_literal_matcher *m);
static bool bx_literal_can_use_avx2(const struct bx_literal_matcher *m);
#if defined(__aarch64__)
static bool bx_literal_can_use_arm64_neon(const struct bx_literal_matcher *m);
static bool bx_literal_can_use_arm64_sve(const struct bx_literal_matcher *m);
#endif
static enum bx_lit_result bx_literal_scan_absent_empty_plan(const struct bx_lit_plan *plan,
                                                            const unsigned char *buf,
                                                            size_t len,
                                                            size_t *match_off);
static enum bx_lit_result bx_literal_scan_absent_byte_plan(const struct bx_lit_plan *plan,
                                                           const unsigned char *buf,
                                                           size_t len,
                                                           size_t *match_off);
static enum bx_lit_result bx_literal_scan_absent_pair_plan(const struct bx_lit_plan *plan,
                                                           const unsigned char *buf,
                                                           size_t len,
                                                           size_t *match_off);
static enum bx_lit_result bx_literal_scan_absent_short_plan(const struct bx_lit_plan *plan,
                                                            const unsigned char *buf,
                                                            size_t len,
                                                            size_t *match_off);
static enum bx_lit_result bx_literal_scan_absent_pair_probe_core(const struct bx_lit_plan *plan,
                                                                 const unsigned char *buf,
                                                                 size_t len,
                                                                 size_t *match_off);
static enum bx_lit_result bx_literal_scan_absent_rare_pair_scalar_plan(
    const struct bx_lit_plan *plan,
    const unsigned char *buf,
    size_t len,
    size_t *match_off);
static enum bx_lit_result bx_literal_scan_absent_long_scalar_plan(const struct bx_lit_plan *plan,
                                                                  const unsigned char *buf,
                                                                  size_t len,
                                                                  size_t *match_off);
static enum bx_lit_result bx_literal_scan_absent_avx2_plan(const struct bx_lit_plan *plan,
                                                           const unsigned char *buf,
                                                           size_t len,
                                                           size_t *match_off);
#if defined(__aarch64__)
#if BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS
static enum bx_lit_result bx_literal_scan_absent_arm64_sve_plan(const struct bx_lit_plan *plan,
                                                                const unsigned char *buf,
                                                                size_t len,
                                                                size_t *match_off);
static enum bx_lit_result bx_literal_scan_absent_arm64_sve_core(const struct bx_lit_plan *plan,
                                                                const unsigned char *buf,
                                                                size_t len,
                                                                size_t *match_off);
#endif
static enum bx_lit_result bx_literal_scan_absent_arm64_neon_plan(const struct bx_lit_plan *plan,
                                                                 const unsigned char *buf,
                                                                 size_t len,
                                                                 size_t *match_off);
static enum bx_lit_result bx_literal_scan_absent_arm64_neon_core(const struct bx_lit_plan *plan,
                                                                 const unsigned char *buf,
                                                                 size_t len,
                                                                 size_t *match_off);
#endif
#if BX_LITERAL_HAVE_AVX2_TARGET
static enum bx_lit_result BX_LITERAL_AVX2_TARGET bx_literal_scan_absent_avx2_core(
    const struct bx_lit_plan *plan,
    const unsigned char *buf,
    size_t len,
    size_t *match_off);
#endif
#if defined(__SSE2__)
static enum bx_lit_result bx_literal_scan_absent_sse2_plan(const struct bx_lit_plan *plan,
                                                           const unsigned char *buf,
                                                           size_t len,
                                                           size_t *match_off);
#endif
static int bx_literal_find_anchored_exact(const struct bx_literal_matcher *m,
                                          const unsigned char *buf,
                                          size_t len,
                                          size_t start,
                                          struct bx_match *out);
static int bx_literal_return_result(int result);
static enum bx_lit_result bx_literal_return_scan_result(enum bx_lit_result result);

struct bx_literal_matcher {
    char  *pattern_lower;
    char  *pattern_raw;
    size_t anchor_index;
    struct bx_lit_plan plan;
    bx_literal_find_fn find;
    bx_literal_case_sensitive_find_fn case_sensitive_find;
    bool   ignore_case;
    bool   has_anchor;
    bool   locale_utf8_upper;
};

/*
 * Backend selection is cold once-per-process state. Per-file scan paths must
 * consume only selected backend/ops state and must not re-enter feature
 * probes, environment override parsing, or backend policy selection.
 */
static pthread_once_t bx_literal_backend_auto_once = PTHREAD_ONCE_INIT;
static enum bx_literal_backend bx_literal_backend_auto_state = BX_LITERAL_BACKEND_SCALAR;
static bool bx_literal_runtime_has_avx2_state = false;
#if defined(__aarch64__)
static bool bx_literal_runtime_has_arm64_neon_state = false;
static bool bx_literal_runtime_has_arm64_sve_state = false;
typedef enum bx_lit_result (*bx_literal_arm64_neon_confirm_lanes_fn)(
    uint16_t lane_mask,
    const unsigned char *buf,
    size_t search_off,
    size_t search_limit,
    size_t pair_offset,
    const unsigned char *needle,
    size_t needle_len,
    size_t *match_off);
/*
 * Keep bitmask lane enumeration as the stable default NEON candidate path.
 * Scalar lane scanning remains available only as a cold compare/diagnostic
 * override so low-candidate no-match workloads can prove the default should
 * stay bitmask-backed.
 */
static bx_literal_arm64_neon_confirm_lanes_fn
    bx_literal_arm64_neon_confirm_candidate_lanes_state =
        bx_literal_arm64_neon_confirm_candidate_lanes_bitmask;
#endif

static void bx_literal_backend_auto_init(void) {
#if defined(__SSE2__)
    const struct bx_literal_x86_probe *probe = bx_literal_x86_probe_get();

    if (bx_literal_x86_probe_has_avx2(probe)) {
        bx_literal_runtime_has_avx2_state = true;
#if defined(__AVX2__)
        bx_literal_backend_auto_state = BX_LITERAL_BACKEND_AVX2;
#else
        bx_literal_backend_auto_state = BX_LITERAL_BACKEND_SSE2;
#endif
        return;
    }
    if (probe && probe->available && probe->max_basic_leaf >= 1u &&
        (probe->leaf1_edx & (1u << 26)) != 0u) {
        bx_literal_backend_auto_state = BX_LITERAL_BACKEND_SSE2;
        return;
    }
#endif
#if defined(__aarch64__)
    /*
     * Prefer NEON as the default AArch64 request on normal ASIMD systems.
     * Eligible rare-pair literals resolve to the NEON backend only on
     * ASIMD-capable systems, but auto dispatch policy must not treat arm64 as
     * scalar-only.
     */
    {
        const struct bx_literal_arm64_probe *probe = bx_literal_arm64_probe_get();
        const char *lane_mode = getenv("BX_SEARCH_LITERAL_ARM64_NEON_LANE_MODE");

        if (!bx_literal_arm64_probe_has_asimd(probe)) {
            bx_literal_backend_auto_state = BX_LITERAL_BACKEND_SCALAR;
            return;
        }
        bx_literal_runtime_has_arm64_neon_state = true;
#if BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS
        if (bx_literal_arm64_probe_has_sve(probe))
            bx_literal_runtime_has_arm64_sve_state = true;
#endif
        if (lane_mode && strcmp(lane_mode, "scalar") == 0) {
            bx_literal_arm64_neon_confirm_candidate_lanes_state =
                bx_literal_arm64_neon_confirm_candidate_lanes_scalar;
        }
        /*
         * Keep NEON as the stable arm64 default even when the probe can report
         * SVE. The SVE literal backend can now be requested explicitly, but
         * auto dispatch should not select it by default until SVE no-match
         * benchmarks prove it is the better stable arm64 path, even on builds
         * where BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS and runtime HWCAP_SVE are both true.
         */
        bx_literal_backend_auto_state = BX_LITERAL_BACKEND_ARM64_NEON;
        return;
    }
#endif
}

static enum bx_literal_backend bx_literal_backend_auto(void) {
    /*
     * Resolve CPU-backed literal dispatch once so matcher compile and scan
     * hot paths consume only the selected backend state.
     */
    pthread_once(&bx_literal_backend_auto_once, bx_literal_backend_auto_init);
    return bx_literal_backend_auto_state;
}

static bool bx_literal_runtime_has_avx2(void) {
    pthread_once(&bx_literal_backend_auto_once, bx_literal_backend_auto_init);
    return bx_literal_runtime_has_avx2_state;
}

#if defined(__aarch64__)
static bool bx_literal_runtime_has_arm64_neon(void) {
    pthread_once(&bx_literal_backend_auto_once, bx_literal_backend_auto_init);
    return bx_literal_runtime_has_arm64_neon_state;
}

static bool bx_literal_runtime_has_arm64_sve(void) {
    pthread_once(&bx_literal_backend_auto_once, bx_literal_backend_auto_init);
    return bx_literal_runtime_has_arm64_sve_state;
}
#endif

static enum bx_literal_backend bx_literal_backend_override(void) {
    const char *value = getenv("BX_SEARCH_LITERAL_BACKEND");

    if (!value || *value == '\0' || strcmp(value, "auto") == 0)
        return bx_literal_backend_auto();
    if (strcmp(value, "scalar") == 0)
        return BX_LITERAL_BACKEND_SCALAR;
    if (strcmp(value, "sve") == 0)
        return BX_LITERAL_BACKEND_ARM64_SVE;
    if (strcmp(value, "neon") == 0)
        return BX_LITERAL_BACKEND_ARM64_NEON;
    if (strcmp(value, "avx2") == 0)
        return BX_LITERAL_BACKEND_AVX2;
    if (strcmp(value, "sse2") == 0)
        return BX_LITERAL_BACKEND_SSE2;
    return bx_literal_backend_auto();
}

static enum bx_literal_backend bx_literal_resolve_backend(
    const struct bx_literal_matcher *m,
    enum bx_literal_backend requested) {
    if (!m)
        return BX_LITERAL_BACKEND_SCALAR;

#if defined(__aarch64__)
    if (requested == BX_LITERAL_BACKEND_ARM64_SVE && bx_literal_can_use_arm64_sve(m))
        return BX_LITERAL_BACKEND_ARM64_SVE;
    if (requested == BX_LITERAL_BACKEND_ARM64_NEON && bx_literal_can_use_arm64_neon(m))
        return BX_LITERAL_BACKEND_ARM64_NEON;
#endif
    if (requested == BX_LITERAL_BACKEND_AVX2 && bx_literal_can_use_avx2(m))
        return BX_LITERAL_BACKEND_AVX2;
#if defined(__SSE2__)
    if (requested == BX_LITERAL_BACKEND_SSE2 && bx_literal_can_use_sse2(m))
        return BX_LITERAL_BACKEND_SSE2;
#else
    (void)requested;
#endif

    return BX_LITERAL_BACKEND_SCALAR;
}

static void bx_literal_choose_anchor(struct bx_literal_matcher *m) {
    size_t needle_len;
    unsigned char rare_byte;
    size_t best_index = 0u;
    size_t best_span = SIZE_MAX;

    if (!m || !m->plan.needle || m->plan.needle_len == 0u)
        return;
    needle_len = m->plan.needle_len;
    rare_byte = m->plan.rare_byte;

    for (size_t i = 0; i < needle_len; ++i) {
        size_t left_span = i;
        size_t right_span = needle_len - i - 1u;
        size_t span = left_span > right_span ? left_span : right_span;

        if (m->plan.needle[i] == rare_byte && span < best_span) {
            best_index = i;
            best_span = span;
        }
    }

    m->anchor_index = best_index;
    m->has_anchor = true;
}

static bool bx_literal_can_use_sse2(const struct bx_literal_matcher *m) {
#if defined(__SSE2__)
    return m && !m->ignore_case && m->plan.needle_len >= 4u && m->plan.needle_len <= 256u;
#else
    (void)m;
    return false;
#endif
}

static bool bx_literal_can_use_avx2(const struct bx_literal_matcher *m) {
#if BX_LITERAL_HAVE_AVX2_TARGET
    return m && !m->ignore_case
        && (m->plan.algo == BX_LIT_SHORT_RARE_PAIR || m->plan.algo == BX_LIT_MEDIUM_RARE_PAIR)
        && m->plan.needle_len >= 4u && m->plan.needle_len <= 256u
        && bx_literal_runtime_has_avx2();
#else
    (void)m;
    return false;
#endif
}

#if defined(__aarch64__)
static bool bx_literal_can_use_arm64_neon(const struct bx_literal_matcher *m) {
    return m && !m->ignore_case
        && (m->plan.algo == BX_LIT_SHORT_RARE_PAIR || m->plan.algo == BX_LIT_MEDIUM_RARE_PAIR)
        && m->plan.needle_len >= 4u && m->plan.needle_len <= 256u
        && bx_literal_runtime_has_arm64_neon();
}

static bool bx_literal_can_use_arm64_sve(const struct bx_literal_matcher *m) {
#if BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS
    return m && !m->ignore_case
        && (m->plan.algo == BX_LIT_SHORT_RARE_PAIR || m->plan.algo == BX_LIT_MEDIUM_RARE_PAIR)
        && m->plan.needle_len >= 4u && m->plan.needle_len <= 256u
        && bx_literal_runtime_has_arm64_sve();
#else
    (void)m;
    return false;
#endif
}

/*
 * Older AArch64 toolchains can provide NEON loads/compares but still miss the
 * vmaxvq_u8 header intrinsic. Keep the fast any-lane probe on vmaxvq_u8 when
 * available and fall back to a small pairwise max reduction otherwise.
 */
static inline bool bx_literal_arm64_neon_any_lane_match(uint8x16_t pair_mask) {
#if BX_LITERAL_HAVE_ARM64_VMAXVQ_U8
    return vmaxvq_u8(pair_mask) != 0u;
#else
    uint8x8_t maxv = vpmax_u8(vget_low_u8(pair_mask), vget_high_u8(pair_mask));

    maxv = vpmax_u8(maxv, maxv);
    maxv = vpmax_u8(maxv, maxv);
    maxv = vpmax_u8(maxv, maxv);
    return vget_lane_u8(maxv, 0) != 0u;
#endif
}

static inline uint16_t bx_literal_arm64_neon_pair_mask_to_bits(uint8x16_t pair_mask) {
    uint8x8_t pair_bytes = vshrn_n_u16(vreinterpretq_u16_u8(pair_mask), 7);
    uint8_t packed_pairs[8];
    uint16_t bits = 0u;

    vst1_u8(packed_pairs, pair_bytes);
    for (size_t pair = 0u; pair < 8u; ++pair)
        bits |= (uint16_t)((packed_pairs[pair] & 0x3u) << (pair * 2u));
    return bits;
}

static enum bx_lit_result bx_literal_arm64_neon_confirm_candidate_lanes_scalar(
    uint16_t lane_mask,
    const unsigned char *buf,
    size_t search_off,
    size_t search_limit,
    size_t pair_offset,
    const unsigned char *needle,
    size_t needle_len,
    size_t *match_off) {
    size_t valid_lanes = search_limit - search_off + 1u;

    for (size_t lane = 0u; lane < valid_lanes; ++lane) {
        size_t pos;

        if ((lane_mask & ((uint16_t)1u << lane)) == 0u)
            continue;
        pos = search_off + lane - pair_offset;
        if (memcmp(buf + pos, needle, needle_len) == 0) {
            if (match_off)
                *match_off = pos;
            return BX_LIT_FOUND;
        }
    }
    return BX_LIT_NOT_FOUND;
}

static enum bx_lit_result bx_literal_arm64_neon_confirm_candidate_lanes_bitmask(
    uint16_t lane_mask,
    const unsigned char *buf,
    size_t search_off,
    size_t search_limit,
    size_t pair_offset,
    const unsigned char *needle,
    size_t needle_len,
    size_t *match_off) {
    size_t valid_lanes = search_limit - search_off + 1u;

    if (valid_lanes < 16u)
        lane_mask &= (uint16_t)(((uint32_t)1u << valid_lanes) - 1u);
    while (lane_mask != 0u) {
        unsigned lane = (unsigned)__builtin_ctz((unsigned)lane_mask);
        size_t pos = search_off + lane - pair_offset;

        if (memcmp(buf + pos, needle, needle_len) == 0) {
            if (match_off)
                *match_off = pos;
            return BX_LIT_FOUND;
        }
        lane_mask &= (uint16_t)(lane_mask - 1u);
    }
    return BX_LIT_NOT_FOUND;
}

#if BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS
static enum bx_lit_result bx_literal_arm64_sve_confirm_candidate_lanes_scalar(
    const unsigned char *candidate_lanes,
    size_t active_lanes,
    const unsigned char *buf,
    size_t search_off,
    size_t pair_offset,
    const unsigned char *needle,
    size_t needle_len,
    size_t *match_off) {
    const unsigned char *cursor = candidate_lanes;
    size_t remaining_lanes = active_lanes;

    while (remaining_lanes != 0u) {
        const unsigned char *candidate = memchr(cursor, 0xff, remaining_lanes);
        size_t pos;
        size_t lane;

        if (!candidate)
            return BX_LIT_NOT_FOUND;
        lane = (size_t)(candidate - candidate_lanes);
        pos = search_off + lane - pair_offset;
        if (memcmp(buf + pos, needle, needle_len) == 0) {
            if (match_off)
                *match_off = pos;
            return BX_LIT_FOUND;
        }
        cursor = candidate + 1u;
        remaining_lanes = active_lanes - (size_t)(cursor - candidate_lanes);
    }
    return BX_LIT_NOT_FOUND;
}
#endif
#endif

int bx_literal_compile(struct bx_literal_matcher **out, const char *pattern, bool ignore_case,
                       bool locale_utf8_upper) {
    size_t plen = strlen(pattern);

    struct bx_literal_matcher *m = calloc(1, sizeof(*m));
    if (!m) return -1;

    m->ignore_case = ignore_case;
    m->locale_utf8_upper = ignore_case && locale_utf8_upper;
    m->pattern_raw = strdup(pattern);
    if (!m->pattern_raw) {
        free(m);
        return -1;
    }
    bx_lit_plan_compile(&m->plan, (const unsigned char *)m->pattern_raw, plen);

    if (ignore_case && !m->locale_utf8_upper) {
        m->pattern_lower = malloc(plen + 1);
        if (!m->pattern_lower) {
            free(m->pattern_raw);
            free(m);
            return -1;
        }
        for (size_t i = 0; i < plen; i++)
            m->pattern_lower[i] = (char)tolower((unsigned char)pattern[i]);
        m->pattern_lower[plen] = '\0';
    } else if (!ignore_case && plen > 0u) {
        bx_literal_choose_anchor(m);
    }
    bx_literal_select_case_sensitive_backend(m);
    bx_search_dev_counters_note_literal_plan_compile();

    *out = m;
    return 0;
}

static bool bx_literal_verify_at_locale_utf8(const struct bx_literal_matcher *m,
                                             const unsigned char *buf,
                                             size_t len,
                                             size_t start,
                                             struct bx_match *out) {
    const unsigned char *needle;
    size_t needle_len;
    size_t pattern_off = 0u;
    size_t input_off = start;

    if (!m || !buf || start > len || m->plan.needle_len == 0u)
        return false;
    needle = m->plan.needle;
    needle_len = m->plan.needle_len;

    while (pattern_off < needle_len) {
        size_t pattern_consume = 0u;
        size_t input_consume = 0u;
        uint32_t pattern_cp = 0u;
        uint32_t input_cp = 0u;

        if (input_off >= len)
            return false;
        if (!bx_rg_decode_utf8_codepoint(needle + pattern_off,
                                         needle_len - pattern_off,
                                         &pattern_consume,
                                         &pattern_cp)) {
            return false;
        }
        if (!bx_rg_decode_utf8_codepoint(buf + input_off, len - input_off,
                                         &input_consume, &input_cp)) {
            return false;
        }
        if (bx_rg_locale_uppercase_codepoint(pattern_cp) !=
            bx_rg_locale_uppercase_codepoint(input_cp)) {
            return false;
        }
        pattern_off += pattern_consume;
        input_off += input_consume;
    }

    if (out) {
        out->start = start;
        out->end = input_off;
    }
    return true;
}

static int bx_literal_find_empty(const unsigned char *buf,
                                 size_t len,
                                 size_t start,
                                 struct bx_match *out) {
    (void)buf;

    bx_search_dev_counters_note_literal_algo_empty_call();
    if (start > len)
        return -1;
    if (out) {
        out->start = start;
        out->end = start;
    }
    return 0;
}

static int bx_literal_find_empty_compiled(const struct bx_literal_matcher *m,
                                          const unsigned char *buf,
                                          size_t len,
                                          size_t start,
                                          struct bx_match *out) {
    (void)m;
    return bx_literal_find_empty(buf, len, start, out);
}

static int bx_literal_find_byte_compiled(const struct bx_literal_matcher *m,
                                         const unsigned char *buf,
                                         size_t len,
                                         size_t start,
                                         struct bx_match *out) {
    if (!m || !buf || start >= len)
        return -1;

    bx_search_dev_counters_note_literal_bytes_scanned(len - start);
    return bx_literal_find_case_sensitive_byte(m, buf, len, start, out);
}

static int bx_literal_find_pair_compiled(const struct bx_literal_matcher *m,
                                         const unsigned char *buf,
                                         size_t len,
                                         size_t start,
                                         struct bx_match *out) {
    if (!m || !buf || start >= len)
        return -1;

    bx_search_dev_counters_note_literal_bytes_scanned(len - start);
    return bx_literal_find_case_sensitive_pair(m, buf, len, start, out);
}

static int bx_literal_find_short_compiled(const struct bx_literal_matcher *m,
                                          const unsigned char *buf,
                                          size_t len,
                                          size_t start,
                                          struct bx_match *out) {
    if (!m || !buf || start >= len)
        return -1;

    bx_search_dev_counters_note_literal_bytes_scanned(len - start);
    return bx_literal_find_case_sensitive_short(m, buf, len, start, out);
}

static int bx_literal_find_case_sensitive_byte(const struct bx_literal_matcher *m,
                                               const unsigned char *buf,
                                               size_t len,
                                               size_t start,
                                               struct bx_match *out) {
    const unsigned char *found;
    size_t pos;

    if (!m || !buf || start >= len || m->plan.needle_len != 1u)
        return -1;

    bx_search_dev_counters_note_literal_algo_byte_call();
    found = memchr(buf + start, m->plan.first_byte, len - start);
    if (!found)
        return -1;

    pos = (size_t)(found - buf);
    if (out) {
        out->start = pos;
        out->end = pos + 1u;
    }
    return 0;
}

static int bx_literal_find_case_sensitive_pair(const struct bx_literal_matcher *m,
                                               const unsigned char *buf,
                                               size_t len,
                                               size_t start,
                                               struct bx_match *out) {
    const unsigned char *needle;
    const unsigned char *cursor;
    const unsigned char *end;
    unsigned char first;
    unsigned char second;
    size_t pos;

    if (!m || !buf || start >= len || m->plan.needle_len != 2u || len - start < 2u)
        return -1;
    needle = m->plan.needle;

    bx_search_dev_counters_note_literal_algo_pair_call();
    first = m->plan.first_byte;
    second = needle[1];
    cursor = buf + start;
    end = buf + len - 1u;
    while (cursor < end) {
        const unsigned char *found = memchr(cursor, first, (size_t)(end - cursor));

        if (!found)
            return -1;
        if (found[1] == second) {
            pos = (size_t)(found - buf);
            if (out) {
                out->start = pos;
                out->end = pos + 2u;
            }
            return 0;
        }
        cursor = found + 1u;
    }
    return -1;
}

static int bx_literal_find_case_sensitive_short(const struct bx_literal_matcher *m,
                                                const unsigned char *buf,
                                                size_t len,
                                                size_t start,
                                                struct bx_match *out) {
    const unsigned char *needle;
    const unsigned char *cursor;
    const unsigned char *end;
    unsigned char first;
    unsigned char second;
    unsigned char third;
    size_t pos;

    if (!m || !buf || start >= len || m->plan.needle_len != 3u || len - start < 3u)
        return -1;
    needle = m->plan.needle;

    bx_search_dev_counters_note_literal_algo_short_call();
    first = m->plan.first_byte;
    second = needle[1];
    third = needle[2];
    cursor = buf + start;
    end = buf + len - 2u;
    while (cursor < end) {
        const unsigned char *found = memchr(cursor, first, (size_t)(end - cursor));

        if (!found)
            return -1;
        if (found[1] != second) {
            cursor = found + 1u;
            continue;
        }
        if (found[2] == third) {
            pos = (size_t)(found - buf);
            if (out) {
                out->start = pos;
                out->end = pos + 3u;
            }
            return 0;
        }
        cursor = found + 1u;
    }
    return -1;
}

static int bx_literal_find_case_sensitive_rare_pair_scalar(const struct bx_literal_matcher *m,
                                                           const unsigned char *buf,
                                                           size_t len,
                                                           size_t start,
                                                           struct bx_match *out) {
    size_t needle_len;
    size_t pair_offset;
    size_t search_off;
    size_t search_limit;
    unsigned char pair_first;
    unsigned char pair_second;

    if (!m || !buf || start >= len || m->plan.needle_len < 4u ||
        m->plan.needle_len > 256u || len - start < m->plan.needle_len) {
        return -1;
    }
    needle_len = m->plan.needle_len;

    bx_search_dev_counters_note_literal_algo_rare_pair_call();
    bx_search_dev_counters_note_literal_algo_scalar_call();
    pair_offset = m->plan.rare_pair_offset;
    pair_first = m->plan.rare_pair_first;
    pair_second = m->plan.rare_pair_second;
    search_off = start + pair_offset;
    search_limit = len - (needle_len - pair_offset);
    while (search_off <= search_limit) {
        const unsigned char *found = memchr(buf + search_off,
                                            pair_first,
                                            search_limit - search_off + 1u);
        size_t pos;

        if (!found)
            return -1;
        pos = (size_t)(found - buf) - pair_offset;
        if (found[1] == pair_second && bx_literal_match_at_anchor(m, buf, pos)) {
            if (out) {
                out->start = pos;
                out->end = pos + needle_len;
            }
            return 0;
        }
        search_off = (size_t)(found - buf) + 1u;
    }
    return -1;
}

static int bx_literal_find_case_sensitive_long_scalar(const struct bx_literal_matcher *m,
                                                      const unsigned char *buf,
                                                      size_t len,
                                                      size_t start,
                                                      struct bx_match *out) {
    bx_search_dev_counters_note_literal_algo_long_call();
    bx_search_dev_counters_note_literal_algo_scalar_call();
    return bx_literal_find_anchored_exact(m, buf, len, start, out);
}

static int bx_literal_find_case_sensitive_avx2(const struct bx_literal_matcher *m,
                                               const unsigned char *buf,
                                               size_t len,
                                               size_t start,
                                               struct bx_match *out) {
#if BX_LITERAL_HAVE_AVX2_TARGET
    size_t match_off = SIZE_MAX;

    if (!m || !buf || start >= len || m->plan.needle_len < 4u ||
        m->plan.needle_len > 256u || len - start < m->plan.needle_len) {
        return -1;
    }

    bx_search_dev_counters_note_literal_algo_rare_pair_call();
    bx_search_dev_counters_note_literal_algo_x86_avx2_call();
    if (bx_literal_scan_absent_avx2_core(&m->plan, buf + start, len - start, &match_off)
        != BX_LIT_FOUND) {
        return -1;
    }
    if (out) {
        out->start = start + match_off;
        out->end = out->start + m->plan.needle_len;
    }
    return 0;
#else
    return bx_literal_find_case_sensitive_rare_pair_scalar(m, buf, len, start, out);
#endif
}

#if defined(__aarch64__)
static int bx_literal_find_case_sensitive_arm64_neon(const struct bx_literal_matcher *m,
                                                     const unsigned char *buf,
                                                     size_t len,
                                                     size_t start,
                                                     struct bx_match *out) {
    size_t match_off = SIZE_MAX;

    if (!m || !buf || start >= len || m->plan.needle_len < 4u ||
        m->plan.needle_len > 256u || len - start < m->plan.needle_len) {
        return -1;
    }

    bx_search_dev_counters_note_literal_algo_rare_pair_call();
    bx_search_dev_counters_note_literal_algo_arm64_neon_call();
    if (bx_literal_scan_absent_arm64_neon_core(&m->plan, buf + start, len - start, &match_off)
        != BX_LIT_FOUND) {
        return -1;
    }
    if (out) {
        out->start = start + match_off;
        out->end = out->start + m->plan.needle_len;
    }
    return 0;
}

#if BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS
static int bx_literal_find_case_sensitive_arm64_sve(const struct bx_literal_matcher *m,
                                                    const unsigned char *buf,
                                                    size_t len,
                                                    size_t start,
                                                    struct bx_match *out) {
    size_t match_off = SIZE_MAX;

    if (!m || !buf || start >= len || m->plan.needle_len < 4u ||
        m->plan.needle_len > 256u || len - start < m->plan.needle_len) {
        return -1;
    }

    bx_search_dev_counters_note_literal_algo_rare_pair_call();
    bx_search_dev_counters_note_literal_algo_arm64_sve_call();
    if (bx_literal_scan_absent_arm64_sve_core(&m->plan, buf + start, len - start, &match_off)
        != BX_LIT_FOUND) {
        return -1;
    }
    if (out) {
        out->start = start + match_off;
        out->end = out->start + m->plan.needle_len;
    }
    return 0;
}
#endif
#endif

static bool bx_literal_match_at_anchor(const struct bx_literal_matcher *m,
                                       const unsigned char *buf,
                                       size_t pos) {
    const unsigned char *needle;
    size_t needle_len;
    size_t prefix_len;
    size_t suffix_off;
    size_t suffix_len;

    if (!m || !buf || !m->plan.needle || !m->has_anchor)
        return false;
    needle = m->plan.needle;
    needle_len = m->plan.needle_len;

    prefix_len = m->anchor_index;
    suffix_off = m->anchor_index + 1u;
    suffix_len = needle_len - suffix_off;
    return (prefix_len == 0u || memcmp(buf + pos, needle, prefix_len) == 0)
        && (suffix_len == 0u
            || memcmp(buf + pos + suffix_off,
                      needle + suffix_off,
                      suffix_len) == 0);
}

static int bx_literal_find_anchored_exact(const struct bx_literal_matcher *m,
                                          const unsigned char *buf,
                                          size_t len,
                                          size_t start,
                                          struct bx_match *out) {
    size_t needle_len;
    size_t search_off;
    size_t search_limit;

    if (!m || !buf || !m->has_anchor || start >= len || len - start < m->plan.needle_len)
        return -1;
    needle_len = m->plan.needle_len;

    search_off = start + m->anchor_index;
    search_limit = len - (needle_len - m->anchor_index);
    while (search_off <= search_limit) {
        const unsigned char *found = memchr(buf + search_off,
                                            m->plan.rare_byte,
                                            search_limit - search_off + 1u);
        size_t pos;

        if (!found)
            return -1;
        pos = (size_t)(found - buf) - m->anchor_index;
        if (bx_literal_match_at_anchor(m, buf, pos)) {
            out->start = pos;
            out->end = pos + needle_len;
            return 0;
        }
        search_off = (size_t)(found - buf) + 1u;
    }
    return -1;
}

static int bx_literal_find_case_sensitive_scalar(const struct bx_literal_matcher *m,
                                                 const unsigned char *buf,
                                                 size_t len,
                                                 size_t start,
                                                 struct bx_match *out) {
    bx_search_dev_counters_note_literal_algo_scalar_call();
    return bx_literal_find_anchored_exact(m, buf, len, start, out);
}

static int bx_literal_find_non_empty(const struct bx_literal_matcher *m,
                                     const unsigned char *buf,
                                     size_t len,
                                     size_t start,
                                     struct bx_match *out) {
    if (!m || !buf || start >= len)
        return -1;

    if (m->ignore_case) {
        if (m->locale_utf8_upper) {
            bx_search_dev_counters_note_literal_bytes_scanned(len - start);
            for (size_t i = start; i < len; i++) {
                if (bx_literal_verify_at_locale_utf8(m, buf, len, i, out))
                    return 0;
            }
            return -1;
        }
        if (len - start < m->plan.needle_len)
            return -1;
        bx_search_dev_counters_note_literal_bytes_scanned(len - start);
        for (size_t i = start; i <= len - m->plan.needle_len; i++) {
            bool match = true;
            for (size_t j = 0; j < m->plan.needle_len; j++) {
                if ((unsigned char)tolower((unsigned char)buf[i + j])
                    != (unsigned char)m->pattern_lower[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                out->start = i;
                out->end = i + m->plan.needle_len;
                return 0;
            }
        }
        return -1;
    }

    if (len - start < m->plan.needle_len)
        return -1;

    bx_search_dev_counters_note_literal_bytes_scanned(len - start);
    return bx_literal_find_case_sensitive_compiled(m, buf, len, start, out);
}

static int bx_literal_find_case_sensitive_compiled(const struct bx_literal_matcher *m,
                                                   const unsigned char *buf,
                                                   size_t len,
                                                   size_t start,
                                                   struct bx_match *out) {
    if (!m || !m->case_sensitive_find)
        return -1;
    return m->case_sensitive_find(m, buf, len, start, out);
}

static const struct bx_lit_ops bx_lit_empty_ops = {
    .scan_absent = bx_literal_scan_absent_empty_plan,
};
static const struct bx_lit_ops bx_lit_byte_ops = {
    .scan_absent = bx_literal_scan_absent_byte_plan,
};
static const struct bx_lit_ops bx_lit_pair_ops = {
    .scan_absent = bx_literal_scan_absent_pair_plan,
};
static const struct bx_lit_ops bx_lit_short_ops = {
    .scan_absent = bx_literal_scan_absent_short_plan,
};
static const struct bx_lit_ops bx_lit_rare_pair_scalar_ops = {
    .scan_absent = bx_literal_scan_absent_rare_pair_scalar_plan,
};
static const struct bx_lit_ops bx_lit_long_scalar_ops = {
    .scan_absent = bx_literal_scan_absent_long_scalar_plan,
};
#if defined(__aarch64__)
static const struct bx_lit_ops bx_lit_rare_pair_arm64_neon_ops = {
    .scan_absent = bx_literal_scan_absent_arm64_neon_plan,
};
#if BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS
static const struct bx_lit_ops bx_lit_rare_pair_arm64_sve_ops = {
    .scan_absent = bx_literal_scan_absent_arm64_sve_plan,
};
#endif
#endif
#if BX_LITERAL_HAVE_AVX2_TARGET
static const struct bx_lit_ops bx_lit_rare_pair_avx2_ops = {
    .scan_absent = bx_literal_scan_absent_avx2_plan,
};
#endif
#if defined(__SSE2__)
static const struct bx_lit_ops bx_lit_rare_pair_sse2_ops = {
    .scan_absent = bx_literal_scan_absent_sse2_plan,
};
#endif

void bx_lit_plan_select_ops(struct bx_lit_plan *plan) {
    if (!plan)
        return;

    plan->ops = NULL;
    switch (plan->algo) {
        case BX_LIT_EMPTY:
            plan->ops = &bx_lit_empty_ops;
            return;
        case BX_LIT_BYTE:
            plan->ops = &bx_lit_byte_ops;
            return;
        case BX_LIT_PAIR:
            plan->ops = &bx_lit_pair_ops;
            return;
        case BX_LIT_SHORT_RARE_PAIR:
            if (plan->needle_len == 3u) {
                plan->ops = &bx_lit_short_ops;
                return;
            }
#if defined(__aarch64__)
#if BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS
            if (plan->backend == BX_LITERAL_BACKEND_ARM64_SVE) {
                plan->ops = &bx_lit_rare_pair_arm64_sve_ops;
                return;
            }
#endif
            if (plan->backend == BX_LITERAL_BACKEND_ARM64_NEON) {
                plan->ops = &bx_lit_rare_pair_arm64_neon_ops;
                return;
            }
#endif
#if BX_LITERAL_HAVE_AVX2_TARGET
            if (plan->backend == BX_LITERAL_BACKEND_AVX2) {
                plan->ops = &bx_lit_rare_pair_avx2_ops;
                return;
            }
#endif
#if defined(__SSE2__)
            if (plan->backend == BX_LITERAL_BACKEND_SSE2) {
                plan->ops = &bx_lit_rare_pair_sse2_ops;
                return;
            }
#endif
            plan->ops = &bx_lit_rare_pair_scalar_ops;
            return;
        case BX_LIT_MEDIUM_RARE_PAIR:
#if defined(__aarch64__)
#if BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS
            if (plan->backend == BX_LITERAL_BACKEND_ARM64_SVE) {
                plan->ops = &bx_lit_rare_pair_arm64_sve_ops;
                return;
            }
#endif
            if (plan->backend == BX_LITERAL_BACKEND_ARM64_NEON) {
                plan->ops = &bx_lit_rare_pair_arm64_neon_ops;
                return;
            }
#endif
#if BX_LITERAL_HAVE_AVX2_TARGET
            if (plan->backend == BX_LITERAL_BACKEND_AVX2) {
                plan->ops = &bx_lit_rare_pair_avx2_ops;
                return;
            }
#endif
#if defined(__SSE2__)
            if (plan->backend == BX_LITERAL_BACKEND_SSE2) {
                plan->ops = &bx_lit_rare_pair_sse2_ops;
                return;
            }
#endif
            plan->ops = &bx_lit_rare_pair_scalar_ops;
            return;
        case BX_LIT_LONG_TWO_WAY:
        case BX_LIT_LONG_HORSPOOL:
            plan->ops = &bx_lit_long_scalar_ops;
            return;
    }
}

static enum bx_lit_result bx_literal_scan_absent_empty_plan(const struct bx_lit_plan *plan,
                                                            const unsigned char *buf,
                                                            size_t len,
                                                            size_t *match_off) {
    (void)plan;
    (void)buf;
    (void)len;

    bx_search_dev_counters_note_literal_algo_empty_call();
    if (match_off)
        *match_off = 0u;
    return BX_LIT_FOUND;
}

static enum bx_lit_result bx_literal_scan_absent_byte_plan(const struct bx_lit_plan *plan,
                                                           const unsigned char *buf,
                                                           size_t len,
                                                           size_t *match_off) {
    const unsigned char *found;

    if (!plan || !buf || plan->needle_len != 1u || len == 0u)
        return BX_LIT_NOT_FOUND;

    bx_search_dev_counters_note_literal_bytes_scanned(len);
    bx_search_dev_counters_note_literal_algo_byte_call();
    found = memchr(buf, plan->first_byte, len);
    if (!found)
        return BX_LIT_NOT_FOUND;

    if (match_off)
        *match_off = (size_t)(found - buf);
    return BX_LIT_FOUND;
}

static enum bx_lit_result bx_literal_scan_absent_pair_plan(const struct bx_lit_plan *plan,
                                                           const unsigned char *buf,
                                                           size_t len,
                                                           size_t *match_off) {
    const unsigned char *cursor;
    const unsigned char *end;
    unsigned char first;
    unsigned char second;

    if (!plan || !buf || plan->needle_len != 2u || len == 0u)
        return BX_LIT_NOT_FOUND;

    bx_search_dev_counters_note_literal_bytes_scanned(len);
    if (len < 2u)
        return BX_LIT_NOT_FOUND;

    bx_search_dev_counters_note_literal_algo_pair_call();
    first = plan->rare_pair_first;
    second = plan->rare_pair_second;
    cursor = buf;
    end = buf + len - 1u;
    while (cursor < end) {
        const unsigned char *found = memchr(cursor, first, (size_t)(end - cursor));

        if (!found)
            return BX_LIT_NOT_FOUND;
        if (found[1] == second) {
            if (match_off)
                *match_off = (size_t)(found - buf);
            return BX_LIT_FOUND;
        }
        cursor = found + 1u;
    }
    return BX_LIT_NOT_FOUND;
}

static enum bx_lit_result bx_literal_scan_absent_short_plan(const struct bx_lit_plan *plan,
                                                            const unsigned char *buf,
                                                            size_t len,
                                                            size_t *match_off) {
    const unsigned char *cursor;
    const unsigned char *end;
    unsigned char first;
    unsigned char second;
    unsigned char third;

    if (!plan || !buf || plan->needle_len != 3u || len == 0u)
        return BX_LIT_NOT_FOUND;

    bx_search_dev_counters_note_literal_bytes_scanned(len);
    if (len < 3u)
        return BX_LIT_NOT_FOUND;

    bx_search_dev_counters_note_literal_algo_short_call();
    first = plan->first_byte;
    second = plan->needle[1];
    third = plan->last_byte;
    cursor = buf;
    end = buf + len - 2u;
    while (cursor < end) {
        const unsigned char *found = memchr(cursor, first, (size_t)(end - cursor));

        if (!found)
            return BX_LIT_NOT_FOUND;
        if (found[1] != second) {
            cursor = found + 1u;
            continue;
        }
        if (found[2] == third) {
            if (match_off)
                *match_off = (size_t)(found - buf);
            return BX_LIT_FOUND;
        }
        cursor = found + 1u;
    }
    return BX_LIT_NOT_FOUND;
}

static enum bx_lit_result bx_literal_scan_absent_pair_probe_core(const struct bx_lit_plan *plan,
                                                                 const unsigned char *buf,
                                                                 size_t len,
                                                                 size_t *match_off) {
    size_t needle_len;
    size_t pair_offset;
    size_t search_off;
    size_t search_limit;
    unsigned char pair_first;
    unsigned char pair_second;

    if (!plan || !buf || !plan->needle || plan->needle_len < 4u || len < plan->needle_len)
        return BX_LIT_NOT_FOUND;
    needle_len = plan->needle_len;
    pair_offset = plan->rare_pair_offset;
    pair_first = plan->rare_pair_first;
    pair_second = plan->rare_pair_second;

    search_off = pair_offset;
    search_limit = len - (needle_len - pair_offset);
    while (search_off <= search_limit) {
        const unsigned char *found = memchr(buf + search_off,
                                            pair_first,
                                            search_limit - search_off + 1u);
        size_t pos;

        if (!found)
            return BX_LIT_NOT_FOUND;
        pos = (size_t)(found - buf) - pair_offset;
        if (found[1] == pair_second &&
            memcmp(buf + pos, plan->needle, needle_len) == 0) {
            if (match_off)
                *match_off = pos;
            return BX_LIT_FOUND;
        }
        search_off = (size_t)(found - buf) + 1u;
    }
    return BX_LIT_NOT_FOUND;
}

static enum bx_lit_result bx_literal_scan_absent_rare_pair_scalar_plan(
    const struct bx_lit_plan *plan,
    const unsigned char *buf,
    size_t len,
    size_t *match_off) {
    if (!plan || !buf || plan->needle_len < 4u || plan->needle_len > 256u ||
        len < plan->needle_len) {
        return BX_LIT_NOT_FOUND;
    }

    bx_search_dev_counters_note_literal_bytes_scanned(len);
    bx_search_dev_counters_note_literal_algo_rare_pair_call();
    bx_search_dev_counters_note_literal_algo_scalar_call();
    return bx_literal_scan_absent_pair_probe_core(plan, buf, len, match_off);
}

static enum bx_lit_result bx_literal_scan_absent_long_scalar_plan(const struct bx_lit_plan *plan,
                                                                  const unsigned char *buf,
                                                                  size_t len,
                                                                  size_t *match_off) {
    if (!plan || !buf || plan->needle_len <= 256u || len < plan->needle_len)
        return BX_LIT_NOT_FOUND;

    bx_search_dev_counters_note_literal_bytes_scanned(len);
    bx_search_dev_counters_note_literal_algo_long_call();
    bx_search_dev_counters_note_literal_algo_scalar_call();
    return bx_literal_scan_absent_pair_probe_core(plan, buf, len, match_off);
}

#if defined(__aarch64__)
#if BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS
static enum bx_lit_result bx_literal_scan_absent_arm64_sve_core(const struct bx_lit_plan *plan,
                                                                const unsigned char *buf,
                                                                size_t len,
                                                                size_t *match_off) {
    const unsigned char *needle;
    size_t needle_len;
    size_t pair_offset;
    size_t search_off;
    size_t search_limit;
    size_t vector_bytes;
    unsigned char pair_first;
    unsigned char pair_second;
    unsigned char candidate_lanes[256u];
    svuint8_t firstv;
    svuint8_t secondv;
    svuint8_t ones;
    svuint8_t zeros;

    if (!plan || !buf || !plan->needle || plan->needle_len < 4u || plan->needle_len > 256u ||
        len < plan->needle_len) {
        return BX_LIT_NOT_FOUND;
    }

    needle = plan->needle;
    needle_len = plan->needle_len;
    pair_offset = plan->rare_pair_offset;
    pair_first = plan->rare_pair_first;
    pair_second = plan->rare_pair_second;
    search_off = pair_offset;
    search_limit = len - (needle_len - pair_offset);
    vector_bytes = svcntb();
    firstv = svdup_n_u8(pair_first);
    secondv = svdup_n_u8(pair_second);
    ones = svdup_n_u8(0xffu);
    zeros = svdup_n_u8(0u);

    while (search_off <= search_limit) {
        size_t active_lanes = search_limit - search_off + 1u;
        svbool_t pg;
        svuint8_t v0;
        svuint8_t v1;
        svbool_t eq0;
        svbool_t eq1;
        svbool_t pair_pg;
        svuint8_t candidate_bytes;

        if (active_lanes > vector_bytes)
            active_lanes = vector_bytes;
        pg = svwhilelt_b8((uint64_t)0, (uint64_t)active_lanes);
        v0 = svld1_u8(pg, buf + search_off);
        v1 = svld1_u8(pg, buf + search_off + 1u);
        eq0 = svcmpeq_u8(pg, v0, firstv);
        eq1 = svcmpeq_u8(pg, v1, secondv);
        pair_pg = svand_b_z(pg, eq0, eq1);
        if (!svptest_any(pg, pair_pg)) {
            search_off += active_lanes;
            continue;
        }
        candidate_bytes = svsel_u8(pair_pg, ones, zeros);
        svst1_u8(pg, candidate_lanes, candidate_bytes);
        if (bx_literal_arm64_sve_confirm_candidate_lanes_scalar(
                candidate_lanes,
                active_lanes,
                buf,
                search_off,
                pair_offset,
                needle,
                needle_len,
                match_off) == BX_LIT_FOUND) {
            return BX_LIT_FOUND;
        }
        search_off += active_lanes;
    }

    return BX_LIT_NOT_FOUND;
}

static enum bx_lit_result bx_literal_scan_absent_arm64_sve_plan(const struct bx_lit_plan *plan,
                                                                const unsigned char *buf,
                                                                size_t len,
                                                                size_t *match_off) {
    if (!plan || !buf || plan->needle_len < 4u || plan->needle_len > 256u || len < plan->needle_len)
        return BX_LIT_NOT_FOUND;

    bx_search_dev_counters_note_literal_bytes_scanned(len);
    bx_search_dev_counters_note_literal_algo_rare_pair_call();
    bx_search_dev_counters_note_literal_algo_arm64_sve_call();
    return bx_literal_scan_absent_arm64_sve_core(plan, buf, len, match_off);
}
#endif

static enum bx_lit_result bx_literal_scan_absent_arm64_neon_core(const struct bx_lit_plan *plan,
                                                                 const unsigned char *buf,
                                                                 size_t len,
                                                                 size_t *match_off) {
    const unsigned char *needle;
    size_t needle_len;
    size_t pair_offset;
    size_t search_off;
    size_t search_limit;
    unsigned char pair_first;
    unsigned char pair_second;
    uint8x16_t firstv;
    uint8x16_t secondv;

    if (!plan || !buf || !plan->needle || plan->needle_len < 4u || plan->needle_len > 256u ||
        len < plan->needle_len) {
        return BX_LIT_NOT_FOUND;
    }

    needle = plan->needle;
    needle_len = plan->needle_len;
    pair_offset = plan->rare_pair_offset;
    pair_first = plan->rare_pair_first;
    pair_second = plan->rare_pair_second;
    search_off = pair_offset;
    search_limit = len - (needle_len - pair_offset);
    firstv = vdupq_n_u8(pair_first);
    secondv = vdupq_n_u8(pair_second);

    while (search_off + 15u <= search_limit && search_off + 16u < len) {
        uint8x16_t v0 = vld1q_u8(buf + search_off);
        uint8x16_t v1 = vld1q_u8(buf + search_off + 1u);
        uint8x16_t eq0 = vceqq_u8(v0, firstv);
        uint8x16_t eq1 = vceqq_u8(v1, secondv);
        uint8x16_t pair_mask = vandq_u8(eq0, eq1);

        if (!bx_literal_arm64_neon_any_lane_match(pair_mask)) {
            search_off += 16u;
            continue;
        }
        {
            uint16_t lane_mask = bx_literal_arm64_neon_pair_mask_to_bits(pair_mask);

            if (bx_literal_arm64_neon_confirm_candidate_lanes_state(
                    lane_mask,
                    buf,
                    search_off,
                    search_limit,
                    pair_offset,
                    needle,
                    needle_len,
                    match_off) == BX_LIT_FOUND) {
                return BX_LIT_FOUND;
            }
        }
        search_off += 16u;
    }

    for (; search_off <= search_limit; ++search_off) {
        size_t pos = search_off - pair_offset;

        if (buf[search_off] == pair_first &&
            buf[search_off + 1u] == pair_second &&
            memcmp(buf + pos, needle, needle_len) == 0) {
            if (match_off)
                *match_off = pos;
            return BX_LIT_FOUND;
        }
    }

    return BX_LIT_NOT_FOUND;
}

static enum bx_lit_result bx_literal_scan_absent_arm64_neon_plan(const struct bx_lit_plan *plan,
                                                                 const unsigned char *buf,
                                                                 size_t len,
                                                                 size_t *match_off) {
    if (!plan || !buf || plan->needle_len < 4u || plan->needle_len > 256u || len < plan->needle_len)
        return BX_LIT_NOT_FOUND;

    bx_search_dev_counters_note_literal_bytes_scanned(len);
    bx_search_dev_counters_note_literal_algo_rare_pair_call();
    bx_search_dev_counters_note_literal_algo_arm64_neon_call();
    return bx_literal_scan_absent_arm64_neon_core(plan, buf, len, match_off);
}
#endif

#if BX_LITERAL_HAVE_AVX2_TARGET
static enum bx_lit_result BX_LITERAL_AVX2_TARGET bx_literal_scan_absent_avx2_core(
    const struct bx_lit_plan *plan,
    const unsigned char *buf,
    size_t len,
    size_t *match_off) {
    const unsigned char *needle;
    size_t needle_len;
    size_t pair_offset;
    size_t search_off;
    size_t search_limit;
    unsigned char pair_first;
    unsigned char pair_second;
    __m256i firstv;
    __m256i secondv;

    if (!plan || !buf || !plan->needle || plan->needle_len < 4u || plan->needle_len > 256u ||
        len < plan->needle_len) {
        return BX_LIT_NOT_FOUND;
    }

    needle = plan->needle;
    needle_len = plan->needle_len;
    pair_offset = plan->rare_pair_offset;
    pair_first = plan->rare_pair_first;
    pair_second = plan->rare_pair_second;
    search_off = pair_offset;
    search_limit = len - (needle_len - pair_offset);
    firstv = _mm256_set1_epi8((char)pair_first);
    secondv = _mm256_set1_epi8((char)pair_second);

    while (search_off + 31u <= search_limit && search_off + 32u < len) {
        __m256i block_first = _mm256_loadu_si256((const __m256i *)(const void *)(buf + search_off));
        __m256i block_second = _mm256_loadu_si256((const __m256i *)(const void *)(buf + search_off + 1u));
        unsigned mask1 = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(block_first, firstv));
        unsigned mask2 = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(block_second, secondv));
        unsigned mask = mask1 & mask2;

        if (mask == 0u) {
            search_off += 32u;
            continue;
        }
        while (mask != 0u) {
            unsigned bit = (unsigned)__builtin_ctz(mask);
            size_t pos = search_off + (size_t)bit - pair_offset;

            if (memcmp(buf + pos, needle, needle_len) == 0) {
                if (match_off)
                    *match_off = pos;
                return BX_LIT_FOUND;
            }
            mask &= mask - 1u;
        }
        search_off += 32u;
    }

    for (; search_off <= search_limit; ++search_off) {
        size_t pos = search_off - pair_offset;

        if (buf[search_off] == pair_first &&
            buf[search_off + 1u] == pair_second &&
            memcmp(buf + pos, needle, needle_len) == 0) {
            if (match_off)
                *match_off = pos;
            return BX_LIT_FOUND;
        }
    }

    return BX_LIT_NOT_FOUND;
}
#endif

static enum bx_lit_result bx_literal_scan_absent_avx2_plan(const struct bx_lit_plan *plan,
                                                           const unsigned char *buf,
                                                           size_t len,
                                                           size_t *match_off) {
#if BX_LITERAL_HAVE_AVX2_TARGET
    if (!plan || !buf || plan->needle_len < 4u || plan->needle_len > 256u || len < plan->needle_len)
        return BX_LIT_NOT_FOUND;

    bx_search_dev_counters_note_literal_bytes_scanned(len);
    bx_search_dev_counters_note_literal_algo_rare_pair_call();
    bx_search_dev_counters_note_literal_algo_x86_avx2_call();
    return bx_literal_scan_absent_avx2_core(plan, buf, len, match_off);
#else
    return bx_literal_scan_absent_rare_pair_scalar_plan(plan, buf, len, match_off);
#endif
}

#if defined(__SSE2__)
static enum bx_lit_result bx_literal_scan_absent_sse2_plan(const struct bx_lit_plan *plan,
                                                           const unsigned char *buf,
                                                           size_t len,
                                                           size_t *match_off) {
    const unsigned char *needle = plan ? plan->needle : NULL;
    size_t needle_len = plan ? plan->needle_len : 0u;
    const unsigned char first = plan ? plan->first_byte : 0u;
    const unsigned char last = plan ? plan->last_byte : 0u;
    size_t i = 0u;
    size_t limit;
    __m128i firstv;
    __m128i lastv;

    if (!plan || !buf || !needle || needle_len < 4u || needle_len > 256u || len < needle_len)
        return BX_LIT_NOT_FOUND;

    bx_search_dev_counters_note_literal_bytes_scanned(len);
    bx_search_dev_counters_note_literal_algo_rare_pair_call();
    bx_search_dev_counters_note_literal_algo_sse2_call();
    limit = len - needle_len;
    firstv = _mm_set1_epi8((char)first);
    lastv = _mm_set1_epi8((char)last);

    while (i + 16u <= limit + 1u) {
        __m128i block_first = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i block_last = _mm_loadu_si128((const __m128i *)(buf + i + needle_len - 1u));
        unsigned mask1 = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(block_first, firstv));
        unsigned mask2 = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(block_last, lastv));
        unsigned mask = mask1 & mask2;

        while (mask != 0u) {
            unsigned bit = (unsigned)__builtin_ctz(mask);
            size_t pos = i + (size_t)bit;
            if (needle_len == 2u ||
                memcmp(buf + pos + 1u, needle + 1u, needle_len - 2u) == 0) {
                if (match_off)
                    *match_off = pos;
                return BX_LIT_FOUND;
            }
            mask &= mask - 1u;
        }
        i += 16u;
    }

    for (; i <= limit; ++i) {
        if (buf[i] == first &&
            buf[i + needle_len - 1u] == last &&
            (needle_len == 2u ||
             memcmp(buf + i + 1u, needle + 1u, needle_len - 2u) == 0)) {
            if (match_off)
                *match_off = i;
            return BX_LIT_FOUND;
        }
    }
    return BX_LIT_NOT_FOUND;
}

static int bx_literal_find_case_sensitive_sse2(const struct bx_literal_matcher *m,
                                               const unsigned char *buf,
                                               size_t len,
                                               size_t start,
                                               struct bx_match *out) {
    const unsigned char *needle = m ? m->plan.needle : NULL;
    size_t needle_len = m ? m->plan.needle_len : 0u;
    bx_search_dev_counters_note_literal_algo_rare_pair_call();
    bx_search_dev_counters_note_literal_algo_sse2_call();
    const unsigned char first = m->plan.first_byte;
    const unsigned char last = m->plan.last_byte;
    size_t i = start;
    size_t limit = len - needle_len;
    __m128i firstv = _mm_set1_epi8((char)first);
    __m128i lastv = _mm_set1_epi8((char)last);

    while (i + 16u <= limit + 1u) {
        __m128i block_first = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i block_last = _mm_loadu_si128((const __m128i *)(buf + i + needle_len - 1u));
        unsigned mask1 = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(block_first, firstv));
        unsigned mask2 = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(block_last, lastv));
        unsigned mask = mask1 & mask2;

        while (mask != 0u) {
            unsigned bit = (unsigned)__builtin_ctz(mask);
            size_t pos = i + (size_t)bit;
            if (needle_len == 2u ||
                memcmp(buf + pos + 1u, needle + 1u, needle_len - 2u) == 0) {
                out->start = pos;
                out->end = pos + needle_len;
                return 0;
            }
            mask &= mask - 1u;
        }
        i += 16u;
    }

    for (; i <= limit; ++i) {
        if (buf[i] == first &&
            buf[i + needle_len - 1u] == last &&
            (needle_len == 2u ||
             memcmp(buf + i + 1u, needle + 1u, needle_len - 2u) == 0)) {
            out->start = i;
            out->end = i + needle_len;
            return 0;
        }
    }
    return -1;
}
#endif

static void bx_literal_select_case_sensitive_backend(struct bx_literal_matcher *m) {
    enum bx_literal_backend requested = bx_literal_backend_override();

    if (!m)
        return;

    m->find = bx_literal_find_non_empty;
    m->plan.backend = BX_LITERAL_BACKEND_SCALAR;
    bx_lit_plan_select_ops(&m->plan);
    m->case_sensitive_find = bx_literal_find_case_sensitive_scalar;
    if (m->plan.algo == BX_LIT_EMPTY) {
        m->find = bx_literal_find_empty_compiled;
        return;
    }
    if (m->ignore_case)
        return;
    if (m->plan.algo == BX_LIT_BYTE) {
        m->find = bx_literal_find_byte_compiled;
        m->case_sensitive_find = bx_literal_find_case_sensitive_byte;
        return;
    }
    if (m->plan.algo == BX_LIT_PAIR) {
        m->find = bx_literal_find_pair_compiled;
        m->case_sensitive_find = bx_literal_find_case_sensitive_pair;
        return;
    }
    if (m->plan.needle_len == 3u) {
        m->find = bx_literal_find_short_compiled;
        m->case_sensitive_find = bx_literal_find_case_sensitive_short;
        return;
    }
    if (m->plan.algo == BX_LIT_SHORT_RARE_PAIR
        || m->plan.algo == BX_LIT_MEDIUM_RARE_PAIR) {
        m->case_sensitive_find = bx_literal_find_case_sensitive_rare_pair_scalar;
        m->plan.backend = bx_literal_resolve_backend(m, requested);
        bx_lit_plan_select_ops(&m->plan);
#if defined(__aarch64__)
        if (m->plan.backend == BX_LITERAL_BACKEND_ARM64_SVE) {
#if BX_LITERAL_HAVE_ARM64_SVE_INTRINSICS
            m->case_sensitive_find = bx_literal_find_case_sensitive_arm64_sve;
#else
            m->case_sensitive_find = bx_literal_find_case_sensitive_rare_pair_scalar;
#endif
            return;
        }
        if (m->plan.backend == BX_LITERAL_BACKEND_ARM64_NEON) {
            m->case_sensitive_find = bx_literal_find_case_sensitive_arm64_neon;
            return;
        }
#endif
#if BX_LITERAL_HAVE_AVX2_TARGET
        if (m->plan.backend == BX_LITERAL_BACKEND_AVX2) {
            m->case_sensitive_find = bx_literal_find_case_sensitive_avx2;
            return;
        }
#endif
#if defined(__SSE2__)
        if (m->plan.backend == BX_LITERAL_BACKEND_SSE2)
            m->case_sensitive_find = bx_literal_find_case_sensitive_sse2;
#endif
        return;
    }
    if (m->plan.algo == BX_LIT_LONG_TWO_WAY
        || m->plan.algo == BX_LIT_LONG_HORSPOOL) {
        m->case_sensitive_find = bx_literal_find_case_sensitive_long_scalar;
        return;
    }
}

static int bx_literal_find_direct(const struct bx_literal_matcher *m,
                                  const unsigned char *buf,
                                  size_t len,
                                  size_t start,
                                  struct bx_match *out) {
    if (!m || !m->find)
        return bx_literal_return_result(-1);
    return bx_literal_return_result(m->find(m, buf, len, start, out));
}

static int bx_literal_return_result(int result) {
    if (result == 0)
        bx_search_dev_counters_note_literal_match();
    else
        bx_search_dev_counters_note_literal_not_found();
    return result;
}

static enum bx_lit_result bx_literal_return_scan_result(enum bx_lit_result result) {
    if (result == BX_LIT_FOUND)
        bx_search_dev_counters_note_literal_match();
    else
        bx_search_dev_counters_note_literal_not_found();
    return result;
}

bool bx_literal_next_candidate(const struct bx_literal_matcher *m,
                               const unsigned char *buf,
                               size_t len,
                               size_t *cursor,
                               size_t *candidate_start) {
    if (!m || !buf || !cursor || !candidate_start)
        return false;
    if (*cursor > len)
        return false;

    struct bx_match match = {0};
    if (bx_literal_find_direct(m, buf, len, *cursor, &match) != 0)
        return false;
    *candidate_start = match.start;
    *cursor = match.start + 1u;
    return true;
}

bool bx_literal_verify_at(const struct bx_literal_matcher *m,
                          const unsigned char *buf,
                          size_t len,
                          size_t start,
                          struct bx_match *out) {
    if (!m || !buf || start > len)
        return false;

    bx_search_dev_counters_note_literal_confirm_call();
    if (m->plan.needle_len == 0u) {
        if (out) {
            out->start = start;
            out->end = start;
        }
        return true;
    }

    if (m->ignore_case) {
        if (m->locale_utf8_upper)
            return bx_literal_verify_at_locale_utf8(m, buf, len, start, out);
        if (len - start < m->plan.needle_len)
            return false;
        for (size_t i = 0; i < m->plan.needle_len; ++i) {
            if ((unsigned char)tolower((unsigned char)buf[start + i])
                != (unsigned char)m->pattern_lower[i]) {
                return false;
            }
        }
    } else if (len - start < m->plan.needle_len ||
               memcmp(buf + start, m->plan.needle, m->plan.needle_len) != 0) {
        return false;
    }

    if (out) {
        out->start = start;
        out->end = start + m->plan.needle_len;
    }
    return true;
}

const char *bx_literal_bytes(const struct bx_literal_matcher *m) {
    return m ? (const char *)m->plan.needle : NULL;
}

size_t bx_literal_len(const struct bx_literal_matcher *m) {
    return m ? m->plan.needle_len : 0u;
}

size_t bx_literal_overlap_len(const struct bx_literal_matcher *m) {
    return m ? m->plan.min_overlap_len : 0u;
}

enum bx_lit_result bx_literal_scan_absent(const struct bx_lit_plan *plan,
                                          const unsigned char *buf,
                                          size_t len,
                                          size_t *match_off) {
    if (!plan || !plan->ops || !plan->ops->scan_absent)
        return BX_LIT_NOT_FOUND;
    if (!buf && !(plan->needle_len == 0u && len == 0u))
        return BX_LIT_NOT_FOUND;

    return bx_literal_return_scan_result(plan->ops->scan_absent(plan, buf, len, match_off));
}

const struct bx_lit_plan *bx_literal_absence_plan(const struct bx_literal_matcher *m) {
    if (!m || m->ignore_case || m->locale_utf8_upper || m->plan.needle_len == 0u)
        return NULL;
    return &m->plan;
}

int bx_literal_find(struct bx_literal_matcher *m, const unsigned char *buf, size_t len,
                    size_t start, struct bx_match *out) {
    const struct bx_lit_plan *absence_plan;

    if (!m)
        return -1;
    absence_plan = bx_literal_absence_plan(m);
    if (absence_plan && buf && start <= len) {
        size_t match_off = SIZE_MAX;

        if (bx_literal_scan_absent(absence_plan, buf + start, len - start, &match_off)
            != BX_LIT_FOUND) {
            return -1;
        }
        if (out) {
            out->start = start + match_off;
            out->end = out->start + absence_plan->needle_len;
        }
        return 0;
    }
    return bx_literal_find_direct(m, buf, len, start, out);
}

bool bx_literal_candidates_are_exact(const struct bx_literal_matcher *m) {
    return m && m->plan.needle_len > 0u && !m->locale_utf8_upper;
}

bool bx_literal_contains_byte(const struct bx_literal_matcher *m, unsigned char byte) {
    if (!m || !m->plan.needle)
        return false;
    return memchr(m->plan.needle, byte, m->plan.needle_len) != NULL;
}

void bx_literal_free(struct bx_literal_matcher *m) {
    if (!m) return;
    free(m->pattern_raw);
    free(m->pattern_lower);
    free(m);
}
