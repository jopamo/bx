#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "find_exec.h"
#include "find_internal.h"
#include "search/walk.h"

static void find_walk_cb(struct walk_entry *entry, void *user) {
    struct find_state *st = user;
    struct find_opts *opts = st->opts;

    if (st->stop && *st->stop)
        return;
    if (entry->depth < opts->min_depth)
        return;
    if (opts->max_depth >= 0 && entry->depth > opts->max_depth)
        return;

    (void)find_eval_expr(st->expr, entry, st);
}

static bool find_expr_argv_has_delete(char **expr_argv, int expr_argc) {
    for (int i = 0; i < expr_argc; i++) {
        if (strcmp(expr_argv[i], "-delete") == 0)
            return true;
    }
    return false;
}

bool find_prepare_expression(const char *progname, struct find_opts *opts,
                             char **expr_argv, int expr_argc,
                             struct find_expr **expr_out) {
    struct find_parser parser = {
        .progname = progname,
        .argv = expr_argv,
        .argc = expr_argc,
        .opts = opts,
        .regex_type = FIND_REGEX_TYPE_DEFAULT,
    };

    struct find_expr *expr = NULL;
    if (expr_argc > 0) {
        expr = find_parse_expr(&parser);
        if (!expr || parser.pos != parser.argc) {
            find_expr_free(expr);
            return false;
        }
    } else {
        expr = find_expr_new(FIND_EXPR_TRUE);
    }

    if (!expr)
        return false;

    if (!parser.explicit_action) {
        struct find_expr *print_expr = find_expr_new(FIND_EXPR_PRINT);
        expr = find_make_binary(FIND_EXPR_AND, expr, print_expr);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", progname);
            return false;
        }
    }

    if (find_expr_argv_has_delete(expr_argv, expr_argc))
        opts->depth_first = true;

    *expr_out = expr;
    return true;
}

int find_run_search(const char *progname, struct find_opts *opts,
                    struct find_expr *expr, char **roots, int root_count) {
    bool stop = false;
    struct find_state st = {
        .progname = progname,
        .opts = opts,
        .expr = expr,
        .stop = &stop,
        .status = 0,
    };
    if (clock_gettime(CLOCK_REALTIME, &st.now) != 0) {
        st.now.tv_sec = time(NULL);
        st.now.tv_nsec = 0;
    }

    struct walk_opts wopts = {
        .hidden = true,
        .no_ignore = true,
        .follow_symlinks = opts->follow_symlinks,
        .follow_root_symlink = opts->follow_root_symlink,
        .post_order = opts->depth_first,
        .stay_on_filesystem = opts->stay_on_filesystem,
        .stop = &stop,
        .suppress_eacces = false,
        .os_error_style = false,
        .error_prefix = progname,
        .max_depth = opts->max_depth,
        .cycle_mode = opts->follow_symlinks ? WALK_CYCLE_DIR_REPEAT
                                            : WALK_CYCLE_NONE,
        .cycle_report = opts->follow_symlinks ? WALK_CYCLE_ERROR
                                              : WALK_CYCLE_IGNORE,
    };

    for (int i = 0; i < root_count && !stop; i++) {
        if (walk_dir(roots[i], &wopts, find_walk_cb, &st) != 0)
            st.status = 1;
    }

    if (find_interrupt_return_code() != 0 && st.status == 0)
        st.status = find_interrupt_return_code();

    if (find_interrupt_return_code() == 0) {
        int pending_rc = find_run_pending_exec_exprs(progname, expr);
        if (pending_rc > 1)
            st.status = pending_rc;
        else if (pending_rc != 0)
            st.status = 1;
    }

    return st.status;
}
