#define _GNU_SOURCE
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "literal.h"

struct bx_literal_matcher {
    char  *pattern_lower;
    char  *pattern_raw;
    size_t plen;
    size_t anchor_index;
    unsigned char anchor_byte;
    bool   ignore_case;
    bool   has_anchor;
};

int bx_literal_compile(struct bx_literal_matcher **out, const char *pattern, bool ignore_case) {
    size_t plen = strlen(pattern);
    if (plen == 0)
        return -1;

    struct bx_literal_matcher *m = calloc(1, sizeof(*m));
    if (!m) return -1;

    m->plen = plen;
    m->ignore_case = ignore_case;
    m->pattern_raw = strdup(pattern);
    if (!m->pattern_raw) {
        free(m);
        return -1;
    }

    if (ignore_case) {
        m->pattern_lower = malloc(plen + 1);
        if (!m->pattern_lower) {
            free(m->pattern_raw);
            free(m);
            return -1;
        }
        for (size_t i = 0; i < plen; i++)
            m->pattern_lower[i] = (char)tolower((unsigned char)pattern[i]);
        m->pattern_lower[plen] = '\0';
    } else if (plen >= 3u) {
        unsigned int counts[UCHAR_MAX + 1u] = {0};
        unsigned int best_count = UINT_MAX;
        size_t best_index = 0u;

        for (size_t i = 0; i < plen; ++i)
            counts[(unsigned char)pattern[i]]++;

        for (size_t i = 0; i < plen; ++i) {
            unsigned char byte = (unsigned char)pattern[i];
            unsigned int count = counts[byte];
            if (count < best_count) {
                best_count = count;
                best_index = i;
                m->anchor_byte = byte;
            }
        }

        if (best_count == 1u) {
            m->anchor_index = best_index;
            m->has_anchor = true;
        }
    }

    *out = m;
    return 0;
}

static int bx_literal_find_direct(const struct bx_literal_matcher *m,
                                  const unsigned char *buf,
                                  size_t len,
                                  size_t start,
                                  struct bx_match *out) {
    if (start >= len || m->plen == 0 || len - start < m->plen)
        return -1;

    if (m->ignore_case) {
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

bool bx_literal_verify_at(const struct bx_literal_matcher *m,
                          const unsigned char *buf,
                          size_t len,
                          size_t start,
                          struct bx_match *out) {
    if (!m || !buf || start > len || m->plen == 0u || len - start < m->plen)
        return false;

    if (m->ignore_case) {
        for (size_t i = 0; i < m->plen; ++i) {
            if ((unsigned char)tolower((unsigned char)buf[start + i])
                != (unsigned char)m->pattern_lower[i]) {
                return false;
            }
        }
    } else if (memcmp(buf + start, m->pattern_raw, m->plen) != 0) {
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
