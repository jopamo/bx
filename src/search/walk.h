#ifndef BX_WALK_WALK_H
#define BX_WALK_WALK_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <time.h>

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
    enum {
        WALK_CYCLE_NONE = 0,
        WALK_CYCLE_DIR_REPEAT,
        WALK_CYCLE_SYMLINK_REPEAT,
    } cycle_mode;
    enum {
        WALK_CYCLE_IGNORE = 0,
        WALK_CYCLE_WARN,
        WALK_CYCLE_ERROR,
    } cycle_report;
};

struct walk_entry {
    char *path;
    bool is_dir;
    bool metadata_loaded;
    bool metadata_tried;
    bool follow_metadata;
    dev_t dev;
    mode_t mode;
    ino_t inode;
    nlink_t nlink;
    uid_t uid;
    gid_t gid;
    off_t size;
    struct timespec atime;
    struct timespec mtime;
    struct timespec ctime;
    int depth;
};

typedef void (*walk_callback)(struct walk_entry *entry, void *user);

bool walk_entry_load_metadata(struct walk_entry *entry);

int walk_dir(const char *root, struct walk_opts *opts, walk_callback cb, void *user);

#endif
