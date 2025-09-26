#ifndef BX_APPLETS_TEXT_FD_EXEC_H
#define BX_APPLETS_TEXT_FD_EXEC_H

#include <stdbool.h>
#include "fd_exec_render.h"
#include "fd_internal.h"

struct fd_exec_items {
    char **v;
    int count;
    int cap;
};

bool fd_exec_items_append_path(struct fd_exec_items *items,
                               const struct fd_render_ctx *ctx,
                               const char *path);
int fd_count_placeholder_args(const struct fd_opts *opts);
void fd_exec_items_free(struct fd_exec_items *items);
int fd_run_exec_commands(const char *progname, const struct fd_opts *opts,
                         struct fd_exec_items *items);

#endif
