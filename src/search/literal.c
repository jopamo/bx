#define _GNU_SOURCE
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include "dev_counters.h"
#include "literal.h"
#include "rg_text.h"

enum bx_literal_backend {
    BX_LITERAL_BACKEND_SCALAR = 0,
    BX_LITERAL_BACKEND_SSE2,
};

typedef int (*bx_literal_case_sensitive_find_fn)(const struct bx_literal_matcher *m,
                                                 const unsigned char *buf,
                                                 size_t len,
                                                 size_t start,
                                                 struct bx_match *out);

static void bx_literal_select_case_sensitive_backend(struct bx_literal_matcher *m);
static bool bx_literal_match_at_anchor(const struct bx_literal_matcher *m,
                                       const unsigned char *buf,
                                       size_t pos);
static int bx_literal_find_anchored_exact(const struct bx_literal_matcher *m,
                                          const unsigned char *buf,
                                          size_t len,
                                          size_t start,
                                          struct bx_match *out);

struct bx_literal_matcher {
    char  *pattern_lower;
    char  *pattern_raw;
    size_t plen;
    size_t anchor_index;
    unsigned char anchor_byte;
    bx_literal_case_sensitive_find_fn case_sensitive_find;
    enum bx_literal_backend backend;
    bool   ignore_case;
    bool   has_anchor;
    bool   locale_utf8_upper;
};

static enum bx_literal_backend bx_literal_backend_override(void) {
    const char *value = getenv("BX_SEARCH_LITERAL_BACKEND");

    if (!value || *value == '\0' || strcmp(value, "auto") == 0)
        return BX_LITERAL_BACKEND_SSE2;
    if (strcmp(value, "scalar") == 0)
        return BX_LITERAL_BACKEND_SCALAR;
    if (strcmp(value, "sse2") == 0)
        return BX_LITERAL_BACKEND_SSE2;
    return BX_LITERAL_BACKEND_SSE2;
}

static void bx_literal_choose_anchor(struct bx_literal_matcher *m) {
    size_t byte_counts[256] = {0};
    size_t best_index = 0u;
    size_t best_count = SIZE_MAX;
    size_t best_span = SIZE_MAX;

    if (!m || !m->pattern_raw || m->plen == 0u)
        return;

    for (size_t i = 0; i < m->plen; ++i) {
        byte_counts[(unsigned char)m->pattern_raw[i]]++;
    }

    for (size_t i = 0; i < m->plen; ++i) {
        size_t count = byte_counts[(unsigned char)m->pattern_raw[i]];
        size_t left_span = i;
        size_t right_span = m->plen - i - 1u;
        size_t span = left_span > right_span ? left_span : right_span;

        if (count < best_count || (count == best_count && span < best_span)) {
            best_index = i;
            best_count = count;
            best_span = span;
        }
    }

    m->anchor_index = best_index;
    m->anchor_byte = (unsigned char)m->pattern_raw[best_index];
    m->has_anchor = true;
}

static bool bx_literal_can_use_sse2(const struct bx_literal_matcher *m) {
#if defined(__SSE2__)
    return m && !m->ignore_case && m->plen >= 2u && m->plen <= 16u;
#else
    (void)m;
    return false;
#endif
}

int bx_literal_compile(struct bx_literal_matcher **out, const char *pattern, bool ignore_case,
                       bool locale_utf8_upper) {
    size_t plen = strlen(pattern);

    struct bx_literal_matcher *m = calloc(1, sizeof(*m));
    if (!m) return -1;

    m->plen = plen;
    m->ignore_case = ignore_case;
    m->locale_utf8_upper = ignore_case && locale_utf8_upper;
    m->pattern_raw = strdup(pattern);
    if (!m->pattern_raw) {
        free(m);
        return -1;
    }

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
    size_t pattern_off = 0u;
    size_t input_off = start;

    if (!m || !buf || start > len || m->plen == 0u)
        return false;

    while (pattern_off < m->plen) {
        size_t pattern_consume = 0u;
        size_t input_consume = 0u;
        uint32_t pattern_cp = 0u;
        uint32_t input_cp = 0u;

        if (input_off >= len)
            return false;
        if (!bx_rg_decode_utf8_codepoint((const unsigned char *)m->pattern_raw + pattern_off,
                                         m->plen - pattern_off,
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

static int bx_literal_find_case_sensitive_byte(const struct bx_literal_matcher *m,
                                               const unsigned char *buf,
                                               size_t len,
                                               size_t start,
                                               struct bx_match *out) {
    const unsigned char *found;
    size_t pos;

    if (!m || !buf || start >= len || m->plen != 1u)
        return -1;

    bx_search_dev_counters_note_literal_algo_byte_call();
    found = memchr(buf + start, (unsigned char)m->pattern_raw[0], len - start);
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
    const unsigned char *cursor;
    const unsigned char *end;
    unsigned char first;
    unsigned char second;
    size_t pos;

    if (!m || !buf || start >= len || m->plen != 2u || len - start < 2u)
        return -1;

    bx_search_dev_counters_note_literal_algo_pair_call();
    first = (unsigned char)m->pattern_raw[0];
    second = (unsigned char)m->pattern_raw[1];
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
    const unsigned char *cursor;
    const unsigned char *end;
    unsigned char first;
    unsigned char second;
    unsigned char third;
    size_t pos;

    if (!m || !buf || start >= len || m->plen != 3u || len - start < 3u)
        return -1;

    bx_search_dev_counters_note_literal_algo_short_call();
    first = (unsigned char)m->pattern_raw[0];
    second = (unsigned char)m->pattern_raw[1];
    third = (unsigned char)m->pattern_raw[2];
    cursor = buf + start;
    end = buf + len - 2u;
    while (cursor < end) {
        const unsigned char *found = memchr(cursor, first, (size_t)(end - cursor));

        if (!found)
            return -1;
        if (found[1] == second && found[2] == third) {
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

static size_t bx_literal_rare_pair_index(const struct bx_literal_matcher *m) {
    if (!m || m->plen < 4u)
        return 0u;
    return (m->anchor_index + 1u < m->plen) ? m->anchor_index : (m->anchor_index - 1u);
}

static int bx_literal_find_case_sensitive_rare_pair_scalar(const struct bx_literal_matcher *m,
                                                           const unsigned char *buf,
                                                           size_t len,
                                                           size_t start,
                                                           struct bx_match *out) {
    size_t pair_index;
    size_t search_off;
    size_t search_limit;
    unsigned char pair_first;
    unsigned char pair_second;

    if (!m || !buf || start >= len || m->plen < 4u || m->plen > 16u || len - start < m->plen)
        return -1;

    bx_search_dev_counters_note_literal_algo_rare_pair_call();
    bx_search_dev_counters_note_literal_algo_scalar_call();
    pair_index = bx_literal_rare_pair_index(m);
    pair_first = (unsigned char)m->pattern_raw[pair_index];
    pair_second = (unsigned char)m->pattern_raw[pair_index + 1u];
    search_off = start + pair_index;
    search_limit = len - (m->plen - pair_index);
    while (search_off <= search_limit) {
        const unsigned char *found = memchr(buf + search_off,
                                            pair_first,
                                            search_limit - search_off + 1u);
        size_t pos;

        if (!found)
            return -1;
        pos = (size_t)(found - buf) - pair_index;
        if (found[1] == pair_second && bx_literal_match_at_anchor(m, buf, pos)) {
            if (out) {
                out->start = pos;
                out->end = pos + m->plen;
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

static bool bx_literal_match_at_anchor(const struct bx_literal_matcher *m,
                                       const unsigned char *buf,
                                       size_t pos) {
    size_t prefix_len;
    size_t suffix_off;
    size_t suffix_len;

    if (!m || !buf || !m->pattern_raw || !m->has_anchor)
        return false;

    prefix_len = m->anchor_index;
    suffix_off = m->anchor_index + 1u;
    suffix_len = m->plen - suffix_off;
    return (prefix_len == 0u || memcmp(buf + pos, m->pattern_raw, prefix_len) == 0)
        && (suffix_len == 0u
            || memcmp(buf + pos + suffix_off,
                      m->pattern_raw + suffix_off,
                      suffix_len) == 0);
}

static int bx_literal_find_anchored_exact(const struct bx_literal_matcher *m,
                                          const unsigned char *buf,
                                          size_t len,
                                          size_t start,
                                          struct bx_match *out) {
    size_t search_off;
    size_t search_limit;

    if (!m || !buf || !m->has_anchor || start >= len || len - start < m->plen)
        return -1;

    search_off = start + m->anchor_index;
    search_limit = len - (m->plen - m->anchor_index);
    while (search_off <= search_limit) {
        const unsigned char *found = memchr(buf + search_off,
                                            m->anchor_byte,
                                            search_limit - search_off + 1u);
        size_t pos;

        if (!found)
            return -1;
        pos = (size_t)(found - buf) - m->anchor_index;
        if (bx_literal_match_at_anchor(m, buf, pos)) {
            out->start = pos;
            out->end = pos + m->plen;
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

#if defined(__SSE2__)
static int bx_literal_find_case_sensitive_sse2(const struct bx_literal_matcher *m,
                                               const unsigned char *buf,
                                               size_t len,
                                               size_t start,
                                               struct bx_match *out) {
    bx_search_dev_counters_note_literal_algo_rare_pair_call();
    bx_search_dev_counters_note_literal_algo_sse2_call();
    const unsigned char first = (unsigned char)m->pattern_raw[0];
    const unsigned char last = (unsigned char)m->pattern_raw[m->plen - 1u];
    size_t i = start;
    size_t limit = len - m->plen;
    __m128i firstv = _mm_set1_epi8((char)first);
    __m128i lastv = _mm_set1_epi8((char)last);

    while (i + 16u <= limit + 1u) {
        __m128i block_first = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i block_last = _mm_loadu_si128((const __m128i *)(buf + i + m->plen - 1u));
        unsigned mask1 = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(block_first, firstv));
        unsigned mask2 = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(block_last, lastv));
        unsigned mask = mask1 & mask2;

        while (mask != 0u) {
            unsigned bit = (unsigned)__builtin_ctz(mask);
            size_t pos = i + (size_t)bit;
            if (m->plen == 2u ||
                memcmp(buf + pos + 1u, m->pattern_raw + 1u, m->plen - 2u) == 0) {
                out->start = pos;
                out->end = pos + m->plen;
                return 0;
            }
            mask &= mask - 1u;
        }
        i += 16u;
    }

    for (; i <= limit; ++i) {
        if (buf[i] == first &&
            buf[i + m->plen - 1u] == last &&
            (m->plen == 2u ||
             memcmp(buf + i + 1u, m->pattern_raw + 1u, m->plen - 2u) == 0)) {
            out->start = i;
            out->end = i + m->plen;
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

    m->backend = BX_LITERAL_BACKEND_SCALAR;
    m->case_sensitive_find = bx_literal_find_case_sensitive_scalar;
    if (m->plen == 1u) {
        m->case_sensitive_find = bx_literal_find_case_sensitive_byte;
        return;
    }
    if (m->plen == 2u) {
        m->case_sensitive_find = bx_literal_find_case_sensitive_pair;
        return;
    }
    if (m->plen == 3u) {
        m->case_sensitive_find = bx_literal_find_case_sensitive_short;
        return;
    }
    if (m->plen <= 16u) {
        m->case_sensitive_find = bx_literal_find_case_sensitive_rare_pair_scalar;
        if (requested == BX_LITERAL_BACKEND_SCALAR)
            return;
        if (!bx_literal_can_use_sse2(m))
            return;
#if defined(__SSE2__)
        m->backend = BX_LITERAL_BACKEND_SSE2;
        m->case_sensitive_find = bx_literal_find_case_sensitive_sse2;
#endif
        return;
    }
    m->case_sensitive_find = bx_literal_find_case_sensitive_long_scalar;
}

static int bx_literal_find_direct(const struct bx_literal_matcher *m,
                                  const unsigned char *buf,
                                  size_t len,
                                  size_t start,
                                  struct bx_match *out) {
    if (m->plen == 0u)
        return bx_literal_find_empty(buf, len, start, out);

    if (start >= len)
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
        if (len - start < m->plen)
            return -1;
        bx_search_dev_counters_note_literal_bytes_scanned(len - start);
        for (size_t i = start; i <= len - m->plen; i++) {
            bool match = true;
            for (size_t j = 0; j < m->plen; j++) {
                if ((unsigned char)tolower((unsigned char)buf[i + j])
                    != (unsigned char)m->pattern_lower[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                out->start = i;
                out->end = i + m->plen;
                return 0;
            }
        }
        return -1;
    }

    if (len - start < m->plen)
        return -1;

    bx_search_dev_counters_note_literal_bytes_scanned(len - start);
    return m->case_sensitive_find
        ? m->case_sensitive_find(m, buf, len, start, out)
        : bx_literal_find_case_sensitive_scalar(m, buf, len, start, out);
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
    if (m->plen == 0u) {
        if (out) {
            out->start = start;
            out->end = start;
        }
        return true;
    }

    if (m->ignore_case) {
        if (m->locale_utf8_upper)
            return bx_literal_verify_at_locale_utf8(m, buf, len, start, out);
        if (len - start < m->plen)
            return false;
        for (size_t i = 0; i < m->plen; ++i) {
            if ((unsigned char)tolower((unsigned char)buf[start + i])
                != (unsigned char)m->pattern_lower[i]) {
                return false;
            }
        }
    } else if (len - start < m->plen || memcmp(buf + start, m->pattern_raw, m->plen) != 0) {
        return false;
    }

    if (out) {
        out->start = start;
        out->end = start + m->plen;
    }
    return true;
}

const char *bx_literal_bytes(const struct bx_literal_matcher *m) {
    return m ? m->pattern_raw : NULL;
}

size_t bx_literal_len(const struct bx_literal_matcher *m) {
    return m ? m->plen : 0u;
}

int bx_literal_find(struct bx_literal_matcher *m, const unsigned char *buf, size_t len,
                    size_t start, struct bx_match *out) {
    if (!m)
        return -1;
    return bx_literal_find_direct(m, buf, len, start, out);
}

bool bx_literal_candidates_are_exact(const struct bx_literal_matcher *m) {
    return m && m->plen > 0u && !m->locale_utf8_upper;
}

bool bx_literal_contains_byte(const struct bx_literal_matcher *m, unsigned char byte) {
    if (!m || !m->pattern_raw)
        return false;
    return memchr(m->pattern_raw, byte, m->plen) != NULL;
}

void bx_literal_free(struct bx_literal_matcher *m) {
    if (!m) return;
    free(m->pattern_raw);
    free(m->pattern_lower);
    free(m);
}
