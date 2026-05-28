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

/*
 * Shared literal plan state.
 *
 * This starts with only the algorithm classification so the storage lives in a
 * shared home before later items add precomputed needle metadata.
 */
struct bx_lit_plan {
    const unsigned char *needle;
    size_t needle_len;
    enum bx_lit_algo algo;
};

void bx_lit_plan_compile(struct bx_lit_plan *plan,
                         const unsigned char *needle,
                         size_t needle_len);

#endif
