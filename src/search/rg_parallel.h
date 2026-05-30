#ifndef BX_SEARCH_RG_PARALLEL_H
#define BX_SEARCH_RG_PARALLEL_H

#include <stdbool.h>
#include <stddef.h>

#include "search.h"

struct bx_search_operand_ref {
    const char *path;
    int index;
};

struct bx_search_exec_plan;
struct bx_search_runtime_snapshot;
struct bx_search_stats;
struct search_opts;

size_t bx_search_rg_thread_count(const struct search_opts *opts);
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
                              const struct bx_search_exec_plan *exec_plan,
                              struct search_opts *opts,
                              const struct bx_search_runtime_snapshot *runtime_snapshot,
                              struct bx_search_stats *stats_out,
                              bool *match_seen_out,
                              bool *error_seen_out);

#endif
