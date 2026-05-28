#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "fd_exec.h"
#include "fd_internal.h"
#include "fd_match.h"
#include "fd_parse.h"
#include "fswalk/walk.h"
#include "search/traverse.h"

static const char *const fd_ignore_filenames[] = {
    ".gitignore",
    ".ignore",
    ".fdignore",
};

int bx_fd_main(int argc, char **argv) {
    struct fd_main_args args;
    if (!fd_parse_main_args(argc, argv, &args)) {
        fd_free_main_args(&args);
        return args.exit_code;
    }

    struct fd_opts *opts = &args.opts;
    const char *progname = args.progname;

    bool stop = false;
    struct bx_walk_opts wopts = {
        .sort_entries = true,
        .follow_symlinks = opts->follow_symlinks,
        .follow_root_symlink = true,
        .stop = &stop,
        .suppress_eacces = true,
        .suppress_errors = false,
        .report_eacces = opts->show_errors,
        .os_error_style = opts->show_errors,
        .error_prefix = opts->show_errors ? "[fd error]" : progname,
        .max_depth = opts->max_depth,
        .cycle_mode = opts->follow_symlinks ? BX_WALK_CYCLE_SYMLINK_REPEAT
                                            : BX_WALK_CYCLE_NONE,
        .cycle_report = BX_WALK_CYCLE_IGNORE,
    };
    struct bx_walk_filter_opts filter_opts = {
        .hidden = opts->hidden,
        .type_filter = opts->type_filter ? opts->type_filter[0] : '\0',
        .exclude_patterns = opts->exclude_patterns,
        .num_exclude_patterns = opts->num_exclude_patterns,
    };
    struct bx_walk_ignore_opts ignore_opts = {
        .no_ignore = opts->no_ignore,
        .no_ignore_parent = opts->no_ignore_parent,
        .no_ignore_vcs = opts->no_ignore_vcs,
        .no_ignore_dot = false,
        .no_require_git = opts->no_require_git,
        .gitignore_enabled = false,
        .ignore_filenames = fd_ignore_filenames,
        .num_ignore_filenames = 3,
    };
    struct bx_search_walk_config walk_config = {
        .walk_opts = &wopts,
        .filter_opts = &filter_opts,
        .ignore_opts = &ignore_opts,
        .visit = fd_walk_callback,
        .error = NULL,
    };

    struct fd_state state;
    memset(&state, 0, sizeof(state));
    if (!fd_state_init(&state, progname, opts, &stop, args.using_implicit_root))
        goto fail;

    int walk_rc = 0;
    for (int i = 0; i < args.search_path_count && !stop; i++) {
        if (bx_search_walk(args.search_paths[i], &walk_config, &state) != 0)
            walk_rc = -1;
    }
    int exec_rc = 0;
    int detail_rc = 0;
    if (!state.exec_collect_failed && walk_rc == 0 &&
        opts->exec_mode != FD_EXEC_NONE) {
        exec_rc = fd_run_exec_commands(progname, opts, &state.exec_items);
    }
    if (!state.output_collect_failed && walk_rc == 0 && opts->list_details)
        detail_rc = fd_detail_items_print(&state.detail_items);
    bool exec_collect_failed = state.exec_collect_failed;
    bool output_collect_failed = state.output_collect_failed;
    fd_state_cleanup(&state);
    fd_free_main_args(&args);
    if (exec_collect_failed || output_collect_failed)
        return 1;
    if (walk_rc != 0)
        return 1;
    if (opts->exec_mode != FD_EXEC_NONE)
        return exec_rc;
    if (opts->list_details)
        return detail_rc;
    if (opts->quiet)
        return opts->results > 0 ? 0 : 1;
    return 0;

fail:
    fd_state_cleanup(&state);
    fd_free_main_args(&args);
    return 1;
}
