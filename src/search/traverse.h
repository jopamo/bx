#ifndef BX_SEARCH_TRAVERSE_H
#define BX_SEARCH_TRAVERSE_H

#include <dirent.h>

#include "fswalk/walk.h"
#include "ignore.h"

struct bx_search_walk_config {
    const struct bx_walk_opts *walk_opts;
    const struct bx_walk_filter_opts *filter_opts;
    const struct bx_walk_ignore_opts *ignore_opts;
    enum bx_walk_action (*visit)(struct bx_walk_entry *entry, void *user);
    enum bx_walk_action (*visit_with_ignore)(struct bx_walk_entry *entry,
                                             const struct bx_ignore_state *ignore_state,
                                             const struct bx_walk_ignore_opts *ignore_opts,
                                             void *user);
    enum bx_walk_action (*error)(const char *path, int errnum, void *user);
    struct bx_ignore_state *inherited_parent_ignore_state;
};

int bx_search_walk(const char *root,
                   const struct bx_search_walk_config *config,
                   void *user);
int bx_search_walk_opened_dir(const char *root,
                              DIR *root_dir,
                              const struct bx_search_walk_config *config,
                              void *user);

#endif
