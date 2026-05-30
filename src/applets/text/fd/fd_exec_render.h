#ifndef BX_APPLETS_TEXT_FD_EXEC_RENDER_H
#define BX_APPLETS_TEXT_FD_EXEC_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include "fd_internal.h"

struct fd_render_ctx {
    const struct fd_opts *opts;
    bool strip_implicit_dot_prefix;
    const char *cwd;
};

void fd_render_ctx_init(struct fd_render_ctx *ctx, const struct fd_opts *opts,
                        bool strip_implicit_dot_prefix, const char *cwd);

size_t fd_placeholder_count(const char *arg);
char *fd_expand_placeholders(const char *arg, const char *path);
char *fd_render_output_path(const struct fd_render_ctx *ctx, const char *path, bool is_dir);
char *fd_render_format_path(const struct fd_render_ctx *ctx, const char *path);
char *fd_render_exec_path(const struct fd_render_ctx *ctx, const char *path);
char *fd_quote_output_path_dup(const struct fd_opts *opts, const char *path);
char *fd_quote_output_path_owned(const struct fd_opts *opts, char *path);
void fd_print_path(const struct fd_render_ctx *ctx, const char *path, bool is_dir);

#endif
