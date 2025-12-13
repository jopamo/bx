#define _GNU_SOURCE
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "literal.h"

struct bx_literal_matcher {
    char  *pattern_lower;
    char  *pattern_raw;
    size_t plen;
    bool   ignore_case;
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

    if (ignore_case) {
        m->pattern_lower = malloc(plen + 1);
        for (size_t i = 0; i < plen; i++)
            m->pattern_lower[i] = (char)tolower((unsigned char)pattern[i]);
        m->pattern_lower[plen] = '\0';
    }

    *out = m;
    return 0;
}

int bx_literal_find(struct bx_literal_matcher *m, const unsigned char *buf, size_t len,
                    size_t start, struct bx_match *out) {
    if (start >= len || m->plen == 0 || len - start < m->plen)
        return -1;

    if (m->ignore_case) {
        for (size_t i = start; i <= len - m->plen; i++) {
            bool match = true;
            for (size_t j = 0; j < m->plen; j++) {
                if (tolower(buf[i + j]) != (unsigned char)m->pattern_lower[j]) {
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
