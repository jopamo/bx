#ifndef BX_WALK_WALK_H
#define BX_WALK_WALK_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>

struct walk_opts {
    bool hidden;
    bool no_ignore;
    bool follow_symlinks;
    bool follow_root_symlink;
    bool post_order;
    bool *stop;
    bool suppress_eacces;
    bool os_error_style;
    const char *error_prefix;
    int  max_depth;
    const char *exclude_pattern;
    const char *type_filter;
    char **exclude_dirs;
    int   num_exclude_dirs;
};

struct walk_entry {
    char *path;
    bool is_dir;
    mode_t mode;
    int depth;
};

typedef void (*walk_callback)(const struct walk_entry *entry, void *user);

int walk_dir(const char *root, struct walk_opts *opts, walk_callback cb, void *user);

#endif
