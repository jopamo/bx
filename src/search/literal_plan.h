#ifndef BX_SEARCH_LITERAL_PLAN_H
#define BX_SEARCH_LITERAL_PLAN_H

#include <stddef.h>

/*
 * Compile-time literal algorithm classification.
 *
 * Keep the enum in shared search mechanics so future plan compilation and
 * backend selection can move out of literal.c without applet policy leakage.
 */
enum bx_lit_algo {
    BX_LIT_EMPTY = 0,
    BX_LIT_BYTE,
    BX_LIT_PAIR,
    BX_LIT_SHORT_RARE_PAIR,
    BX_LIT_MEDIUM_RARE_PAIR,
    BX_LIT_LONG_TWO_WAY,
    BX_LIT_LONG_HORSPOOL,
};

enum bx_literal_backend {
    BX_LITERAL_BACKEND_SCALAR = 0,
    BX_LITERAL_BACKEND_SSE2,
};

/*
 * Shared compile-once literal plan state.
 *
 * Keep hot-path metadata here so literal.c runtime paths can consume selected
 * bytes and algorithm choices without re-deriving them per matcher/search.
 */
struct bx_lit_plan {
    const unsigned char *needle;
    size_t needle_len;
    size_t min_overlap_len;
    size_t rare_pair_offset;
    unsigned char first_byte;
    unsigned char last_byte;
    unsigned char rare_byte;
    unsigned char rare_pair_first;
    unsigned char rare_pair_second;
    enum bx_literal_backend backend;
    enum bx_lit_algo algo;
};

void bx_lit_plan_compile(struct bx_lit_plan *plan,
                         const unsigned char *needle,
                         size_t needle_len);

#endif
