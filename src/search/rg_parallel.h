#ifndef BX_SEARCH_RG_PARALLEL_H
#define BX_SEARCH_RG_PARALLEL_H

#include <stdbool.h>

#include "search.h"

struct bx_search_operand_ref {
    const char *path;
    int index;
};

struct bx_search_stats;
struct search_opts;

bool bx_search_parallel_rg_supported(enum bx_search_personality personality,
                                     const struct search_opts *opts,
                                     int num_files,
                                     bool rg_searches_stdin);
int bx_search_run_parallel_rg(int argc,
                              char **argv,
                              int first_file,
                              struct bx_search_operand_ref *sorted_operands,
                              int sorted_operand_count,
                              const char *progname,
                              const char *pattern,
                              enum bx_search_personality personality,
                              struct search_opts *opts,
                              struct bx_search_stats *stats_out,
                              bool *match_seen_out,
                              bool *error_seen_out);

#endif
