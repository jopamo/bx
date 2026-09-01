#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pattern_vec.h"

static bool bx_search_pattern_vec_reserve(struct bx_search_pattern_vec *vec,
                                          size_t needed) {
    char **items;
    bool *casefold;
    bool *is_type;
    size_t cap;

    if (!vec)
        return false;
    if (needed <= vec->cap)
        return true;

    cap = vec->cap == 0u ? 8u : vec->cap;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2u)
            return false;
        cap *= 2u;
    }
    if (cap > SIZE_MAX / sizeof(*items))
        return false;

    items = calloc(cap, sizeof(*items));
    casefold = calloc(cap, sizeof(*casefold));
    is_type = calloc(cap, sizeof(*is_type));
    if (!items || !casefold || !is_type) {
        free(items);
        free(casefold);
        free(is_type);
        return false;
    }
    if (vec->len > 0u) {
        memcpy(items, vec->items, vec->len * sizeof(*items));
        memcpy(casefold, vec->casefold, vec->len * sizeof(*casefold));
        memcpy(is_type, vec->is_type, vec->len * sizeof(*is_type));
    }
    free(vec->items);
    free(vec->casefold);
    free(vec->is_type);
    vec->items = items;
    vec->casefold = casefold;
    vec->is_type = is_type;
    vec->cap = cap;
    return true;
}

bool bx_search_pattern_vec_append(struct bx_search_pattern_vec *vec,
                                  const char *pattern,
                                  bool casefold,
                                  bool is_type) {
    size_t len;
    char *copy;

    if (!vec || !pattern)
        return false;
    len = strlen(pattern);
    if (len == SIZE_MAX || vec->bytes > SIZE_MAX - (len + 1u))
        return false;

    copy = malloc(len + 1u);
    if (!copy)
        return false;
    memcpy(copy, pattern, len + 1u);
    if (!bx_search_pattern_vec_reserve(vec, vec->len + 1u)) {
        free(copy);
        return false;
    }

    vec->items[vec->len] = copy;
    vec->casefold[vec->len] = casefold;
    vec->is_type[vec->len] = is_type;
    vec->len++;
    vec->bytes += len + 1u;
    return true;
}

bool bx_search_pattern_vec_clone(struct bx_search_pattern_vec *dest,
                                 const struct bx_search_pattern_vec *src) {
    if (!dest || !src)
        return false;
    for (size_t i = 0u; i < src->len; ++i) {
        if (!bx_search_pattern_vec_append(dest, src->items[i],
                                          src->casefold[i], src->is_type[i])) {
            bx_search_pattern_vec_dispose(dest);
            return false;
        }
    }
    return true;
}

void bx_search_pattern_vec_drop_last(struct bx_search_pattern_vec *vec) {
    if (vec && vec->len > 0u)
        vec->len--;
}

void bx_search_pattern_vec_dispose(struct bx_search_pattern_vec *vec) {
    if (!vec)
        return;
    for (size_t i = 0u; i < vec->cap; ++i)
        free(vec->items[i]);
    free(vec->items);
    free(vec->casefold);
    free(vec->is_type);
    memset(vec, 0, sizeof(*vec));
}
