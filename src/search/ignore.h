#ifndef BX_SEARCH_IGNORE_H
#define BX_SEARCH_IGNORE_H

#include <stdbool.h>

struct walk_opts;

bool bx_ignore_append_pattern(char ***patterns, int *n, int *cap, const char *pattern);

void bx_ignore_free_patterns(char **patterns, int n);

bool bx_ignore_append_pattern(char ***patterns, int *n, int *cap, const char *pattern);

bool bx_ignore_clone_patterns(char **src, int src_n,
                              char ***dst, int *dst_n, int *dst_cap);

bool bx_ignore_load_patterns(const char *dirpath, const struct walk_opts *opts,
                             char ***patterns, int *n);

bool bx_ignore_load_parent_patterns(const char *root, const struct walk_opts *opts,
                                    char ***patterns, int *n);

bool bx_ignore_path_ignored(const char *name, char **patterns, int n);

bool bx_ignore_enable_gitignore_for_root(const char *root, const struct walk_opts *opts);

#endif
