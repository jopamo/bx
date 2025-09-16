#ifndef BX_SEARCH_FILTER_H
#define BX_SEARCH_FILTER_H

#include <stdbool.h>

struct bx_ignore_state;
struct walk_opts;

struct bx_walk_filter_state {
    const struct walk_opts *opts;
    const char *root_path;
};

void bx_walk_filter_init(struct bx_walk_filter_state *state,
                         const struct walk_opts *opts,
                         const char *root_path);

bool bx_walk_filter_should_skip(const struct bx_walk_filter_state *state,
                                const char *name,
                                const char *path,
                                const struct bx_ignore_state *ignore_state);

#endif
