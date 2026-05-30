#ifndef BX_APPLETS_TEXT_FD_INTERNAL_H
#define BX_APPLETS_TEXT_FD_INTERNAL_H

#include <stdbool.h>

#define FD_MAX_AND_PATTERNS 16
#define FD_PLACEHOLDER "{}"
#define FD_MAX_EXCLUDE_PATTERNS 16

enum fd_exec_mode {
    FD_EXEC_NONE = 0,
    FD_EXEC_EACH,
    FD_EXEC_BATCH,
};

enum fd_strip_cwd_prefix_mode {
    FD_STRIP_CWD_PREFIX_UNSET = 0,
    FD_STRIP_CWD_PREFIX_AUTO,
    FD_STRIP_CWD_PREFIX_ALWAYS,
    FD_STRIP_CWD_PREFIX_NEVER,
};

enum fd_placeholder_kind {
    FD_PH_NONE = 0,
    FD_PH_PATH,
    FD_PH_BASENAME,
    FD_PH_DIRNAME,
    FD_PH_PATH_STEM,
    FD_PH_BASENAME_STEM,
};

struct fd_opts {
    bool hidden;
    bool no_ignore;
    bool no_ignore_parent;
    bool no_ignore_vcs;
    bool no_require_git;
    bool follow_symlinks;
    bool absolute_path;
    bool full_path;
    bool ignore_case;
    bool smart_case;
    bool case_sensitive;
    bool fixed_strings;
    bool glob_match;
    bool print0;
    bool terminal_quote_paths;
    bool quiet;
    bool list_details;
    bool show_errors;
    bool show_type;
    const char *path_separator;
    const char *output_format;
    enum fd_strip_cwd_prefix_mode strip_cwd_prefix;
    const char *pattern;
    const char *and_patterns[FD_MAX_AND_PATTERNS];
    int num_and_patterns;
    char *exclude_patterns[FD_MAX_EXCLUDE_PATTERNS];
    int num_exclude_patterns;
    const char *type_filter;
    const char *extension;
    int max_depth;
    int min_depth;
    int exact_depth;
    int max_results;
    int results;
    int unrestrict_level;
    int batch_size;
    bool batch_size_set;
    enum fd_exec_mode exec_mode;
    const char **exec_argv;
    int exec_argc;
};

#endif
