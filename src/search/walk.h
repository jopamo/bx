#ifndef BX_WALK_WALK_H
#define BX_WALK_WALK_H

#include <stdbool.h>
#include <stddef.h>

struct walk_opts {
    bool hidden;
    bool no_ignore;
    bool follow_symlinks;
    int  max_depth;
    const char *exclude_pattern;
    const char *type_filter;
};

struct walk_entry {
    char *path;
    bool is_dir;
};

typedef void (*walk_callback)(const struct walk_entry *entry, void *user);

int walk_dir(const char *root, struct walk_opts *opts, walk_callback cb, void *user);

#endif
