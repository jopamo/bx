#ifndef BX_SEARCH_OPTIONS_H
#define BX_SEARCH_OPTIONS_H

#include <stdbool.h>
#include "search.h"
#include "lib/color.h"

#define MAX_INCLUDE_PATTERNS 32
#define MAX_EXCLUDE_PATTERNS 32
#define MAX_EXCLUDE_DIR_PATTERNS 32
#define MAX_CUSTOM_TYPES 16
#define MAX_CLEARED_TYPES 32

struct search_opts {
    bool show_line_number;
    bool show_byte_offset;
    bool show_filename;
    bool hide_filename;
    bool invert_match;
    bool count_only;
    bool files_with_matches;
    bool files_without_match;
    bool quiet;
    bool ignore_case;
    bool smart_case;
    bool only_matching;
    bool fixed_strings;
    bool extended_regex;
    bool perl_regexp;
    bool word_regexp;
    bool line_regexp;
    bool files_only;
    int  max_count;
    int  unrestrict_level;
    char *extra_patterns[16];
    int   num_extra_patterns;
    enum  bx_color_mode color_mode;
    int  after_context;
    int  before_context;
    bool recursive;
    bool follow_symlinks;
    int  num_include;
    char *include_patterns[MAX_INCLUDE_PATTERNS];
    bool include_pattern_casefold[MAX_INCLUDE_PATTERNS];
    int  num_exclude;
    char *exclude_patterns[MAX_EXCLUDE_PATTERNS];
    int  num_exclude_dir;
    char *exclude_dir_patterns[MAX_EXCLUDE_DIR_PATTERNS];
    char *custom_type_names[MAX_CUSTOM_TYPES];
    char *custom_type_globs[MAX_CUSTOM_TYPES];
    int   num_custom_types;
    char *cleared_type_names[MAX_CLEARED_TYPES];
    int   num_cleared_types;
    bool binary_as_text;
    bool binary_without_match;
    bool hidden;
    bool no_ignore;
    bool no_ignore_parent;
    bool no_ignore_vcs;
    bool no_ignore_dot;
    bool no_require_git;
    bool null_output;
    bool null_filename;
    bool null_data;
    bool multiline;
    bool multiline_dotall;
    bool stop_on_nonmatch;
    bool pcre2_version;
    enum {
        BX_RG_ENGINE_UNSPECIFIED = 0,
        BX_RG_ENGINE_DEFAULT,
        BX_RG_ENGINE_PCRE2,
        BX_RG_ENGINE_AUTO,
    } rg_engine;
    bool suppress_group_separator;
    char *label;
    char *group_separator;
    int  max_depth;
    enum {
        BX_GREP_DIR_DEFAULT = 0,
        BX_GREP_DIR_READ,
        BX_GREP_DIR_RECURSE,
        BX_GREP_DIR_SKIP,
    } directory_mode;
};

int bx_search_parse_options(int argc, char **argv, struct search_opts *opts,
                             enum bx_search_personality personality,
                             const char **pattern, int *first_file);

void bx_search_free_options(struct search_opts *opts);

void bx_search_print_help(const char *progname);
void bx_search_print_version(const char *progname);
void bx_search_print_type_list(void);

#endif
