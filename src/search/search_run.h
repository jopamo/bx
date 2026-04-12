#ifndef BX_SEARCH_SEARCH_RUN_H
#define BX_SEARCH_SEARCH_RUN_H

#include <stdbool.h>

#include "search.h"

struct bx_search_plan;
struct bx_search_stats;
struct search_opts;

struct bx_search_run_args {
    int argc;
    char **argv;
    int first_file;
    const char *pattern;
    const char *progname;
    enum bx_search_personality personality;
    const struct bx_search_plan *plan;
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
