#ifndef BX_SEARCH_TRAVERSE_H
#define BX_SEARCH_TRAVERSE_H

#include "fswalk/walk.h"

struct bx_search_walk_config {
    const struct bx_walk_opts *walk_opts;
    const struct bx_walk_filter_opts *filter_opts;
    const struct bx_walk_ignore_opts *ignore_opts;
    enum bx_walk_action (*visit)(struct bx_walk_entry *entry, void *user);
    enum bx_walk_action (*error)(const char *path, int errnum, void *user);
};

int bx_search_walk(const char *root,
                   const struct bx_search_walk_config *config,
                   void *user);

#endif
