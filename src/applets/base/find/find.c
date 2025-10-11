#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "find_internal.h"

void find_report_error(const char *progname, const char *path, int errnum) {
    fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errnum));
}

int bx_find_main(int argc, char **argv) {
    const char *progname = argv[0] ? argv[0] : "find";
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        find_print_help(progname);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        find_print_version(progname);
        return 0;
    }

    struct find_opts opts = {
        .max_depth = -1,
        .min_depth = 0,
    };

    struct find_root_list root_list = {0};
    char **roots = NULL;
    int root_count = 0;
    char **expr_argv = NULL;
    int expr_argc = 0;
    if (!find_parse_command_line(progname, argc, argv, &opts, &root_list,
                                 &roots, &root_count, &expr_argv,
                                 &expr_argc)) {
        find_root_list_free(&root_list);
        return 1;
    }

    struct find_expr *expr = NULL;
    if (!find_prepare_expression(progname, &opts, expr_argv, expr_argc, &expr)) {
        free(expr_argv);
        find_root_list_free(&root_list);
        return 1;
    }

    int status = find_run_search(progname, &opts, expr, roots, root_count);
    find_expr_free(expr);
    free(expr_argv);
    find_root_list_free(&root_list);
    return status;
}
