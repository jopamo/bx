#ifndef BX_SEARCH_OPTIONS_H
#define BX_SEARCH_OPTIONS_H

#include <stdbool.h>
#include "search.h"

#define MAX_INCLUDE_PATTERNS 32
#define MAX_EXCLUDE_PATTERNS 32
#define MAX_EXCLUDE_DIR_PATTERNS 32

struct search_opts {
    bool show_line_number;
    bool show_filename;
    bool hide_filename;
    bool invert_match;
    bool count_only;
    bool files_with_matches;
    bool files_without_match;
    bool quiet;
    bool ignore_case;
    bool only_matching;
    bool fixed_strings;
    bool extended_regex;
    int  after_context;
    int  before_context;
    bool recursive;
    bool follow_symlinks;
    int  num_include;
    char *include_patterns[MAX_INCLUDE_PATTERNS];
    int  num_exclude;
    char *exclude_patterns[MAX_EXCLUDE_PATTERNS];
    int  num_exclude_dir;
    char *exclude_dir_patterns[MAX_EXCLUDE_DIR_PATTERNS];
    bool binary_as_text;
    bool binary_without_match;
};

int bx_search_parse_options(int argc, char **argv, struct search_opts *opts,
                             enum bx_search_personality personality,
                             const char **pattern, int *first_file);

void bx_search_free_options(struct search_opts *opts);

void bx_search_print_help(const char *progname);
void bx_search_print_version(const char *progname);

#endif
