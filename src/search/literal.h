#ifndef BX_REGEX_LITERAL_H
#define BX_REGEX_LITERAL_H

#include <stdbool.h>
#include <stddef.h>
#include "pcre2_matcher.h"

struct bx_literal_matcher;

int bx_literal_compile(struct bx_literal_matcher **out, const char *pattern, bool ignore_case,
                       bool locale_utf8_upper);
bool bx_literal_next_candidate(const struct bx_literal_matcher *m,
                               const unsigned char *buf,
                               size_t len,
                               size_t *cursor,
                               size_t *candidate_start);
bool bx_literal_verify_at(const struct bx_literal_matcher *m,
                          const unsigned char *buf,
                          size_t len,
                          size_t start,
                          struct bx_match *out);
const char *bx_literal_bytes(const struct bx_literal_matcher *m);
size_t bx_literal_len(const struct bx_literal_matcher *m);
size_t bx_literal_overlap_len(const struct bx_literal_matcher *m);
int bx_literal_find(struct bx_literal_matcher *m, const unsigned char *buf, size_t len,
                    size_t start, struct bx_match *out);
bool bx_literal_candidates_are_exact(const struct bx_literal_matcher *m);
bool bx_literal_contains_byte(const struct bx_literal_matcher *m, unsigned char byte);

static inline bool bx_literal_match_crosses_chunk_boundary(const struct bx_match *match,
                                                           size_t carry_len) {
    return match && carry_len > 0u && match->start < carry_len && match->end > carry_len;
}

void bx_literal_free(struct bx_literal_matcher *m);

#endif
