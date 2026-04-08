#ifndef BX_SEARCH_SORT_H
#define BX_SEARCH_SORT_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "options.h"
#include "search.h"

struct bx_search_sorted_path {
    char *path;
    bool strip_dot_prefix;
    struct timespec sort_time;
    size_t sequence;
};

struct bx_search_sorted_paths {
    struct bx_search_sorted_path *items;
    size_t len;
    size_t cap;
};

bool bx_search_sort_requested(const struct search_opts *opts);
bool bx_search_sort_is_path(const struct search_opts *opts);
bool bx_search_sort_is_metadata(const struct search_opts *opts);
bool bx_search_sort_is_descending(const struct search_opts *opts);

void bx_search_sorted_paths_dispose(struct bx_search_sorted_paths *paths);

int bx_search_collect_metadata_sorted_paths(int argc,
                                            char **argv,
                                            int first_file,
                                            const char *progname,
                                            enum bx_search_personality personality,
                                            const struct search_opts *opts,
                                            struct bx_search_sorted_paths *out,
                                            bool *error_seen);

#endif
