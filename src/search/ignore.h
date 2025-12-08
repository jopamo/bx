#ifndef BX_SEARCH_IGNORE_H
#define BX_SEARCH_IGNORE_H

#include <stdbool.h>

#include "fswalk/walk.h"

struct bx_ignore_state {
    struct bx_ignore_state *parent;
    const char *dirpath;
    char *owned_dirpath;
    const char *root_prefix;
    char *owned_root_prefix;
    char **patterns;
    int pattern_count;
};

bool bx_ignore_append_pattern(char ***patterns, int *n, int *cap, const char *pattern);

void bx_ignore_free_patterns(char **patterns, int n);

void bx_ignore_state_init(struct bx_ignore_state *state,
                          struct bx_ignore_state *parent,
                          const char *dirpath,
                          char **patterns, int pattern_count);

void bx_ignore_state_dispose(struct bx_ignore_state *state);

void bx_ignore_state_dispose_chain(struct bx_ignore_state *state);

bool bx_ignore_load_patterns(const char *dirpath, const struct bx_walk_ignore_opts *opts,
                             char ***patterns, int *n);

struct bx_ignore_state *bx_ignore_load_parent_state(const char *root,
                                                    const struct bx_walk_ignore_opts *opts,
                                                    bool *ok);

bool bx_ignore_state_matches_path(const struct bx_ignore_state *state,
                                  const char *name,
                                  const char *path,
                                  const char *root_relative_path);

bool bx_ignore_path_ignored(const char *name, char **patterns, int n);

bool bx_ignore_enable_gitignore_for_root(const char *root, const struct bx_walk_ignore_opts *opts);

#endif
