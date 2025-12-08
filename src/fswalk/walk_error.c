#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "walk_internal.h"

bool bx_walk_should_stop(const struct bx_walk_opts *opts) {
    return opts && opts->stop && *opts->stop;
}

const char *bx_walk_error_prefix(const struct bx_walk_opts *opts) {
    return (opts && opts->error_prefix) ? opts->error_prefix : "walk";
}

void bx_walk_report_error(const struct bx_walk_opts *opts, const char *path, int errnum) {
    if (opts && opts->suppress_errors)
        return;

    if (opts && opts->os_error_style) {
        fprintf(stderr, "%s: %s: %s (os error %d)\n",
                bx_walk_error_prefix(opts), path, strerror(errnum), errnum);
        return;
    }

    fprintf(stderr, "%s: %s: %s\n",
            bx_walk_error_prefix(opts), path, strerror(errnum));
}

void bx_walk_report_loop(const struct bx_walk_opts *opts, const char *path) {
    if (!opts || opts->cycle_report == BX_WALK_CYCLE_IGNORE)
        return;
    if (opts->suppress_errors)
        return;

    if (opts->os_error_style) {
        bx_walk_report_error(opts, path, ELOOP);
        return;
    }

    if (opts->cycle_report == BX_WALK_CYCLE_WARN) {
        fprintf(stderr, "%s: %s: warning: recursive directory loop\n",
                bx_walk_error_prefix(opts), path);
        return;
    }

    fprintf(stderr, "%s: %s: file system loop detected\n",
            bx_walk_error_prefix(opts), path);
}

enum bx_walk_action bx_walk_handle_error(const struct bx_walk_ctx *ctx,
                                         const char *path,
                                         int errnum) {
    if (ctx->ops->error)
        return ctx->ops->error(path, errnum, ctx->user);

    bx_walk_report_error(ctx->opts, path, errnum);
    return BX_WALK_ERROR;
}

enum bx_walk_action bx_walk_apply_visit_action(const struct bx_walk_ctx *ctx,
                                               struct bx_walk_entry *entry,
                                               int *status_out) {
    enum bx_walk_action action = ctx->ops->visit(entry, ctx->user);

    if (entry->prune && action == BX_WALK_CONTINUE)
        action = BX_WALK_PRUNE;

    switch (action) {
    case BX_WALK_CONTINUE:
        break;
    case BX_WALK_PRUNE:
        entry->prune = true;
        break;
    case BX_WALK_STOP:
        if (ctx->opts->stop)
            *ctx->opts->stop = true;
        break;
    case BX_WALK_ERROR:
        if (status_out)
            *status_out = -1;
        break;
    }

    return action;
}
