#define _GNU_SOURCE
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include "literal.h"
#include "rg_text.h"

struct bx_literal_matcher {
    char  *pattern_lower;
    char  *pattern_raw;
    size_t plen;
    size_t anchor_index;
    unsigned char anchor_byte;
    bool   ignore_case;
    bool   has_anchor;
    bool   locale_utf8_upper;
};

int bx_literal_compile(struct bx_literal_matcher **out, const char *pattern, bool ignore_case,
                       bool locale_utf8_upper) {
    size_t plen = strlen(pattern);
    if (plen == 0)
        return -1;

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
    }

    /*
     * Keep case-sensitive literals on libc memmem. The older anchor-byte
     * candidate path lost to libc on current rg tiny-file workloads.
     */

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

static int bx_literal_find_direct(const struct bx_literal_matcher *m,
                                  const unsigned char *buf,
                                  size_t len,
                                  size_t start,
                                  struct bx_match *out) {
    if (start >= len || m->plen == 0)
        return -1;

    if (m->ignore_case) {
        if (m->locale_utf8_upper) {
            for (size_t i = start; i < len; i++) {
                if (bx_literal_verify_at_locale_utf8(m, buf, len, i, out))
                    return 0;
            }
            return -1;
        }
        if (len - start < m->plen)
            return -1;
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

#if defined(__SSE2__)
    if (m->plen >= 2u && m->plen <= 16u) {
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

    void *found = memmem(buf + start, len - start, m->pattern_raw, m->plen);
    if (!found)
        return -1;

    out->start = (size_t)((unsigned char *)found - buf);
    out->end = out->start + m->plen;
    return 0;
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

    if (!m->has_anchor || m->ignore_case) {
        struct bx_match match = {0};
        if (bx_literal_find_direct(m, buf, len, *cursor, &match) != 0)
            return false;
        *candidate_start = match.start;
        *cursor = match.start + 1u;
        return true;
    }

    if (m->plen == 0u || len < m->plen || *cursor > len - m->plen)
        return false;

    size_t search_off = *cursor + m->anchor_index;
    size_t search_limit = len - (m->plen - m->anchor_index);
    if (search_off > search_limit)
        return false;

    const unsigned char *found = memchr(buf + search_off,
                                        m->anchor_byte,
                                        search_limit - search_off + 1u);
    if (!found)
        return false;

    *candidate_start = (size_t)(found - buf) - m->anchor_index;
    *cursor = *candidate_start + 1u;
    return true;
}

bool bx_literal_next_scanner_candidate(const struct bx_literal_matcher *m,
                                       const unsigned char *buf,
                                       size_t len,
                                       size_t *cursor,
                                       size_t *candidate_start) {
    if (!m || !buf || !cursor || !candidate_start)
        return false;
    if (*cursor > len)
        return false;

    if (!m->has_anchor && !m->ignore_case) {
        if (len - *cursor < m->plen)
            return false;

        void *found = memmem(buf + *cursor, len - *cursor, m->pattern_raw, m->plen);
        if (!found)
            return false;

        *candidate_start = (size_t)((const unsigned char *)found - buf);
        *cursor = *candidate_start + 1u;
        return true;
    }

    return bx_literal_next_candidate(m, buf, len, cursor, candidate_start);
}

bool bx_literal_verify_at(const struct bx_literal_matcher *m,
                          const unsigned char *buf,
                          size_t len,
                          size_t start,
                          struct bx_match *out) {
    if (!m || !buf || start > len || m->plen == 0u)
        return false;

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

size_t bx_literal_len(const struct bx_literal_matcher *m) {
    return m ? m->plen : 0u;
}

int bx_literal_find(struct bx_literal_matcher *m, const unsigned char *buf, size_t len,
                    size_t start, struct bx_match *out) {
    if (!m)
        return -1;
    if (!m->has_anchor || m->ignore_case)
        return bx_literal_find_direct(m, buf, len, start, out);

    size_t cursor = start;
    size_t candidate_start = 0u;
    while (bx_literal_next_candidate(m, buf, len, &cursor, &candidate_start)) {
        if (bx_literal_verify_at(m, buf, len, candidate_start, out))
            return 0;
    }
    return -1;
}

bool bx_literal_candidates_are_exact(const struct bx_literal_matcher *m) {
    return m && !m->has_anchor && !m->locale_utf8_upper;
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
