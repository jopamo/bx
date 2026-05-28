#include "literal_plan.h"

static enum bx_lit_algo bx_lit_plan_select_algo(size_t needle_len) {
    if (needle_len == 0u)
        return BX_LIT_EMPTY;
    if (needle_len == 1u)
        return BX_LIT_BYTE;
    if (needle_len == 2u)
        return BX_LIT_PAIR;
    if (needle_len <= 16u)
        return BX_LIT_SHORT_RARE_PAIR;
    return BX_LIT_LONG_TWO_WAY;
}

void bx_lit_plan_compile(struct bx_lit_plan *plan,
                         const unsigned char *needle,
                         size_t needle_len) {
    if (!plan)
        return;

    plan->needle = needle;
    plan->needle_len = needle_len;
    plan->algo = bx_lit_plan_select_algo(needle_len);
}
