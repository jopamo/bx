#ifndef BX_SEARCH_SEARCH_RUN_H
#define BX_SEARCH_SEARCH_RUN_H

#include <stdbool.h>

#include "search.h"

struct bx_search_plan;
struct bx_search_runtime_snapshot;
struct bx_search_stats;
struct search_opts;

struct bx_search_run_args {
    int argc;
    char **argv;
    int first_file;
    const char *pattern;
    const char *progname;
    enum bx_search_personality personality;
    /* Immutable output/orchestration policy published before workers start. */
    const struct bx_search_plan *plan;
    /*
     * Published before any recursive/parallel worker starts. The caller owns
     * the snapshot and may destroy it only after bx_search_run returns.
     */
    const struct bx_search_runtime_snapshot *runtime_snapshot;
    /*
     * Coordinator-owned mutable options. bx_search_run may finalize them before
     * workers start; worker backends then borrow them read-only.
     */
    struct search_opts *opts;
    struct bx_search_stats *stats;
};

struct bx_search_run_result {
    int status;
    bool ran_search;
};

bool bx_search_run_should_search_stdin(void);
void bx_search_run(const struct bx_search_run_args *args,
                   struct bx_search_run_result *result);

#endif
