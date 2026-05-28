#include <stdint.h>

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
    if (needle_len <= 256u)
        return BX_LIT_MEDIUM_RARE_PAIR;
    return BX_LIT_LONG_TWO_WAY;
}

static size_t bx_lit_plan_select_rare_byte_index(const unsigned char *needle,
                                                 size_t needle_len) {
    size_t byte_counts[256] = {0};
    size_t best_index = 0u;
    size_t best_count = SIZE_MAX;
    size_t best_span = SIZE_MAX;

    if (!needle || needle_len == 0u)
        return 0u;

    for (size_t i = 0; i < needle_len; ++i) {
        byte_counts[needle[i]]++;
    }

    for (size_t i = 0; i < needle_len; ++i) {
        size_t count = byte_counts[needle[i]];
        size_t left_span = i;
        size_t right_span = needle_len - i - 1u;
        size_t span = left_span > right_span ? left_span : right_span;

        if (count < best_count || (count == best_count && span < best_span)) {
            best_index = i;
            best_count = count;
            best_span = span;
        }
    }

    return best_index;
}

static size_t bx_lit_plan_select_rare_pair_offset(size_t rare_index, size_t needle_len) {
    if (needle_len < 2u)
        return 0u;
    return (rare_index + 1u < needle_len) ? rare_index : (rare_index - 1u);
}

void bx_lit_plan_compile(struct bx_lit_plan *plan,
                         const unsigned char *needle,
                         size_t needle_len) {
    size_t rare_index;
    size_t rare_pair_offset;

    if (!plan)
        return;

    rare_index = bx_lit_plan_select_rare_byte_index(needle, needle_len);
    rare_pair_offset = bx_lit_plan_select_rare_pair_offset(rare_index, needle_len);
    plan->needle = needle;
    plan->needle_len = needle_len;
    plan->min_overlap_len = needle_len > 0u ? needle_len - 1u : 0u;
    plan->rare_pair_offset = rare_pair_offset;
    plan->first_byte = (needle && needle_len > 0u) ? needle[0] : 0u;
    plan->last_byte = (needle && needle_len > 0u) ? needle[needle_len - 1u] : 0u;
    plan->rare_byte = (needle && needle_len > 0u) ? needle[rare_index] : 0u;
    plan->rare_pair_first = (needle && needle_len > 1u) ? needle[rare_pair_offset] : 0u;
    plan->rare_pair_second = (needle && needle_len > 1u) ? needle[rare_pair_offset + 1u] : 0u;
    plan->backend = BX_LITERAL_BACKEND_SCALAR;
    plan->algo = bx_lit_plan_select_algo(needle_len);
}
