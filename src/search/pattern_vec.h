#ifndef BX_SEARCH_PATTERN_VEC_H
#define BX_SEARCH_PATTERN_VEC_H

#include <stdbool.h>
#include <stddef.h>

struct bx_search_pattern_vec {
    char **items;
    bool *casefold;
    bool *is_type;
    size_t len;
    size_t cap;
    size_t bytes;
};

bool bx_search_pattern_vec_append(struct bx_search_pattern_vec *vec,
                                  const char *pattern,
                                  bool casefold,
                                  bool is_type);
bool bx_search_pattern_vec_clone(struct bx_search_pattern_vec *dest,
                                 const struct bx_search_pattern_vec *src);
void bx_search_pattern_vec_drop_last(struct bx_search_pattern_vec *vec);
void bx_search_pattern_vec_dispose(struct bx_search_pattern_vec *vec);

#endif
