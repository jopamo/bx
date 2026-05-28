#ifndef BX_FSWALK_WALK_H
#define BX_FSWALK_WALK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

enum bx_walk_counter {
    BX_WALK_COUNTER_DIRENTS_SEEN = 0,
    BX_WALK_COUNTER_DIRS_SEEN,
    BX_WALK_COUNTER_FILES_SEEN,
    BX_WALK_COUNTER_SYMLINKS_SEEN,
    BX_WALK_COUNTER_UNKNOWN_DTYPE_SEEN,
    BX_WALK_COUNTER_LSTAT_CALLS,
    BX_WALK_COUNTER_FSTATAT_CALLS,
    BX_WALK_COUNTER_OPENAT_CALLS,
    BX_WALK_COUNTER_PATH_JOIN_CALLS,
    BX_WALK_COUNTER_PATH_ALLOCS,
    BX_WALK_COUNTER_PATH_COPIES_BEFORE_MATCH,
};

struct bx_walk_counter_ops {
    void (*note)(enum bx_walk_counter counter, uint64_t count, void *user);
    void *user;
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
    const struct bx_walk_counter_ops *counter_ops;
};

struct bx_walk_filter_opts {
    bool hidden;
    bool glob_case_insensitive;
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
    bool no_ignore_exclude;
    bool no_ignore_files;
    bool no_ignore_global;
    bool no_require_git;
    bool gitignore_enabled;
    bool ignore_file_case_insensitive;
    bool suppress_ignore_messages;
    bool os_error_style;
    const char *error_prefix;
    const char *git_root;
    char *const *extra_ignore_files;
    int num_extra_ignore_files;
    const char *const *ignore_filenames;
    int num_ignore_filenames;
};

struct bx_walk_entry {
    char *path;
    unsigned char d_type;
    bool d_type_known;
    bool is_dir;
    bool is_symlink;
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
    const struct bx_walk_counter_ops *counter_ops;
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
