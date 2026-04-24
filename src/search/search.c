#include <stdbool.h>
#include <stdio.h>

#include "dev_counters.h"
#include "ignore.h"
#include "options.h"
#include "search.h"
#include "search_internal.h"
#include "search_plan.h"
#include "search_run.h"

static int finish_search_main(int status) {
    bx_search_dev_counters_report(stderr);
    bx_search_dev_counters_reset();
    return status;
}

int bx_search_main(int argc, char **argv, enum bx_search_personality personality) {
    struct search_opts opts;
    struct bx_search_plan plan = {0};
    const char *pattern;
    int first_file;
    const char *progname = argv[0] ? argv[0] : "grep";

    bx_search_dev_counters_begin_from_env();

    int rc = bx_search_parse_options(argc, argv, &opts, personality, &pattern, &first_file);
    if (rc != 0) {
        bx_search_free_options(&opts);
        if (rc == 1)
            return finish_search_main(0);
        if (rc == 3)
            return finish_search_main(1);
        return finish_search_main(2);
    }

    if (opts.line_buffered) {
        setvbuf(stdout, NULL, _IOLBF, 0);
    } else if (opts.block_buffered) {
        setvbuf(stdout, NULL, _IOFBF, BUFSIZ);
    }

    if (personality == BX_SEARCH_RG) {
        struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(progname, &opts);
        bx_ignore_validate_explicit_ignore_files(&ignore_opts);
    }

    int num_files = argc - first_file;
    bool rg_searches_stdin = (personality == BX_SEARCH_RG
                              && !opts.files_only
                              && num_files == 0
                              && bx_search_run_should_search_stdin());
    bx_search_plan_build(&plan, personality, &opts, num_files, rg_searches_stdin);
    if (personality == BX_SEARCH_RG && bx_search_plan_debug_enabled())
        bx_search_plan_debug_dump(stderr, &plan);
    struct bx_search_stats stats = {0};
    struct bx_search_output_ctx main_output_ctx = {
        .out = stdout,
        .err = stderr,
        .stats = opts.stats ? &stats : NULL,
    };
    struct bx_search_output_ctx *previous_output_ctx = bx_search_output_ctx_push(&main_output_ctx);

    struct bx_search_run_args run_args = {
        .argc = argc,
        .argv = argv,
        .first_file = first_file,
        .pattern = pattern,
        .progname = progname,
        .personality = personality,
        .plan = &plan,
        .opts = &opts,
        .stats = &stats,
    };
    struct bx_search_run_result run_result = {0};

    bx_search_run(&run_args, &run_result);
    if (opts.stats && run_result.ran_search)
        bx_search_print_stats_summary(&stats);
    bx_search_output_ctx_pop(previous_output_ctx);
    bx_search_free_options(&opts);
    return finish_search_main(run_result.status);
}
