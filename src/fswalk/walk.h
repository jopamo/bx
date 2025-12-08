#ifndef BX_FSWALK_WALK_H
#define BX_FSWALK_WALK_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

enum bx_walk_cycle_mode {
    BX_WALK_CYCLE_NONE = 0,
    BX_WALK_CYCLE_DIR_REPEAT,
    BX_WALK_CYCLE_SYMLINK_REPEAT,
};

enum bx_walk_cycle_report {
    BX_WALK_CYCLE_IGNORE = 0,
    BX_WALK_CYCLE_WARN,
    BX_WALK_CYCLE_ERROR,
};

struct bx_walk_opts {
    bool sort_entries;
    bool reverse_sort;
    bool follow_symlinks;
    bool follow_root_symlink;
    bool post_order;
    bool stay_on_filesystem;
    bool *stop;
    bool suppress_eacces;
    bool suppress_errors;
    bool report_eacces;
    bool os_error_style;
    const char *error_prefix;
    int max_depth;
    enum bx_walk_cycle_mode cycle_mode;
    enum bx_walk_cycle_report cycle_report;
};

struct bx_walk_filter_opts {
    bool hidden;
    char *const *include_patterns;
    const bool *include_pattern_casefold;
    int num_include_patterns;
    char *const *exclude_patterns;
    int num_exclude_patterns;
    char *const *exclude_dirs;
    int num_exclude_dirs;
};

struct bx_walk_ignore_opts {
    bool no_ignore;
    bool no_ignore_parent;
    bool no_ignore_vcs;
    bool no_ignore_dot;
    bool no_require_git;
    bool gitignore_enabled;
    const char *const *ignore_filenames;
    int num_ignore_filenames;
};

struct bx_walk_entry {
    char *path;
    bool is_dir;
    bool prune;
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
    blksize_t block_size;
    struct timespec atime;
    struct timespec mtime;
    struct timespec ctime;
    int depth;
};

enum bx_walk_action {
    BX_WALK_CONTINUE = 0,
    BX_WALK_PRUNE,
    BX_WALK_STOP,
    BX_WALK_ERROR,
};

struct bx_walk_ops {
    enum bx_walk_action (*visit)(struct bx_walk_entry *entry, void *user);
    enum bx_walk_action (*error)(const char *path, int errnum, void *user);
};

bool bx_walk_entry_load_metadata(struct bx_walk_entry *entry);
int bx_walk(const char *root,
            const struct bx_walk_opts *opts,
            const struct bx_walk_ops *ops,
            void *user);

#endif
