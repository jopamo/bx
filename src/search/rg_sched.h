#ifndef BX_SEARCH_RG_SCHED_H
#define BX_SEARCH_RG_SCHED_H

#include <stdbool.h>

#include "search.h"

struct bx_search_operand_ref;
struct bx_search_exec_plan;
struct bx_search_stats;
struct search_opts;

bool bx_rg_sched_supported(enum bx_search_personality personality,
                           const struct search_opts *opts,
                           int num_files,
                           bool rg_searches_stdin);
int bx_rg_sched_run(int argc,
                    char **argv,
                    int first_file,
                    struct bx_search_operand_ref *sorted_operands,
                    int sorted_operand_count,
                    const char *progname,
                    const char *pattern,
                    enum bx_search_personality personality,
                    const struct bx_search_exec_plan *exec_plan,
                    struct search_opts *opts,
                    size_t thread_count,
                    struct bx_search_stats *stats_out,
                    bool *match_seen_out,
                    bool *error_seen_out);

#endif
