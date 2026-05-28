#include <stdint.h>

#include "dev_counters.h"
#include "literal_plan.h"

/*
 * Estimated byte frequency for source/text-heavy trees.
 *
 * Lower values mean "rarer in typical text", so tie-breaks in rare-byte
 * selection can bias toward bytes that are also uncommon in the haystack.
 */
static const uint8_t bx_lit_plan_source_text_byte_frequency[256] = {
      1,   4,   4,   4,   4,   4,   4,   4,   4, 140, 220,   6,  10,  80,   6,   6,
      6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,   6,
    255, 140, 150, 170,  70, 120, 150, 120, 180, 180, 155, 165, 165, 160, 185, 190,
    150, 145, 140, 138, 136, 134, 132, 130, 128, 126, 175, 170, 150, 180, 150,  80,
     70, 195, 100, 145, 150, 210, 125, 115, 165, 185,  50,  80, 160, 130, 180, 190,
    120,  40, 170, 175, 200, 140,  90, 110,  60, 105,  35, 180, 130, 180,  70, 210,
     90, 235, 140, 185, 190, 250, 165, 155, 205, 225,  90, 120, 200, 170, 220, 230,
    160,  80, 210, 215, 240, 180, 130, 150, 100, 145,  75, 180, 145, 180,  60,   4,
     24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
     24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
     24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
     24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
     24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
     24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
     24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
     24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,  24,
};

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
    size_t best_text_frequency = SIZE_MAX;
    size_t best_span = SIZE_MAX;

    if (!needle || needle_len == 0u)
        return 0u;

    for (size_t i = 0; i < needle_len; ++i) {
        byte_counts[needle[i]]++;
    }

    for (size_t i = 0; i < needle_len; ++i) {
        size_t count = byte_counts[needle[i]];
        size_t text_frequency = bx_lit_plan_source_text_byte_frequency[needle[i]];
        size_t left_span = i;
        size_t right_span = needle_len - i - 1u;
        size_t span = left_span > right_span ? left_span : right_span;

        if (count < best_count
            || (count == best_count && text_frequency < best_text_frequency)
            || (count == best_count && text_frequency == best_text_frequency
                && span < best_span)) {
            best_index = i;
            best_count = count;
            best_text_frequency = text_frequency;
            best_span = span;
        }
    }

    return best_index;
}

static size_t bx_lit_plan_score_adjacent_pair(unsigned char first, unsigned char second) {
    return (size_t)bx_lit_plan_source_text_byte_frequency[first]
         + (size_t)bx_lit_plan_source_text_byte_frequency[second];
}

static size_t bx_lit_plan_select_rare_pair_offset(const unsigned char *needle,
                                                  size_t needle_len) {
    size_t best_pair_offset = 0u;
    size_t best_pair_score = SIZE_MAX;
    size_t best_pair_span = SIZE_MAX;

    if (!needle || needle_len < 2u)
        return 0u;

    for (size_t i = 0; i + 1u < needle_len; ++i) {
        size_t pair_score = bx_lit_plan_score_adjacent_pair(needle[i], needle[i + 1u]);
        size_t left_span = i;
        size_t right_span = needle_len - i - 2u;
        size_t span = left_span > right_span ? left_span : right_span;

        if (pair_score < best_pair_score
            || (pair_score == best_pair_score && span < best_pair_span)) {
            best_pair_offset = i;
            best_pair_score = pair_score;
            best_pair_span = span;
        }
    }

    return best_pair_offset;
}

void bx_lit_plan_compile(struct bx_lit_plan *plan,
                         const unsigned char *needle,
                         size_t needle_len) {
    size_t rare_index;
    size_t rare_pair_offset;

    if (!plan)
        return;

    rare_index = bx_lit_plan_select_rare_byte_index(needle, needle_len);
    rare_pair_offset = bx_lit_plan_select_rare_pair_offset(needle, needle_len);
    bx_search_dev_counters_note_literal_selected_pair_distribution(rare_pair_offset, needle_len);
    plan->needle = needle;
    plan->needle_len = needle_len;
    plan->min_overlap_len = needle_len > 0u ? needle_len - 1u : 0u;
    plan->rare_pair_offset = rare_pair_offset;
    plan->first_byte = (needle && needle_len > 0u) ? needle[0] : 0u;
    plan->last_byte = (needle && needle_len > 0u) ? needle[needle_len - 1u] : 0u;
    plan->rare_byte = (needle && needle_len > 0u) ? needle[rare_index] : 0u;
    plan->rare_pair_first = (needle && needle_len > 1u) ? needle[rare_pair_offset] : 0u;
    plan->rare_pair_second = (needle && needle_len > 1u) ? needle[rare_pair_offset + 1u] : 0u;
    plan->ops = NULL;
    plan->backend = BX_LITERAL_BACKEND_SCALAR;
    plan->algo = bx_lit_plan_select_algo(needle_len);
}
