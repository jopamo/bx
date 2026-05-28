#ifndef BX_SEARCH_FILTER_H
#define BX_SEARCH_FILTER_H

#include <stdbool.h>

struct bx_ignore_state;
#include "fswalk/walk.h"

struct bx_walk_filter_state {
    const struct bx_walk_filter_opts *opts;
    const char *root_path;
    size_t root_path_len;
};

void bx_walk_filter_init(struct bx_walk_filter_state *state,
                         const struct bx_walk_filter_opts *opts,
                         const char *root_path);

bool bx_walk_filter_matches_include(const struct bx_walk_filter_state *state,
                                    const char *name,
                                    const char *path);

bool bx_walk_filter_should_skip(const struct bx_walk_filter_state *state,
                                struct bx_walk_entry *entry,
                                const struct bx_ignore_state *ignore_state,
                                bool *entry_selected_out);

#endif
