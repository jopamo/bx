#ifndef BX_APPLETS_TEXT_FD_OUTPUT_H
#define BX_APPLETS_TEXT_FD_OUTPUT_H

#include <stdbool.h>
#include "fd_exec_render.h"
#include "fd_internal.h"
#include "fswalk/walk.h"

struct bx_line_writer;

struct fd_detail_item {
    char *display_path;
    char *symlink_target;
    mode_t mode;
    nlink_t nlink;
    uid_t uid;
    gid_t gid;
    off_t size;
    time_t mtime_sec;
};

struct fd_detail_items {
    struct fd_detail_item *v;
    const struct fd_opts *render_opts;
    int count;
    int cap;
};

bool fd_print_match_output(struct bx_line_writer *writer,
                           const struct fd_render_ctx *ctx,
                           const struct fd_opts *opts,
                           const char *path, bool is_dir);
bool fd_detail_items_append(struct fd_detail_items *items,
                            const struct fd_render_ctx *ctx,
                            struct bx_walk_entry *entry);
int fd_detail_items_print(struct fd_detail_items *items,
                          struct bx_line_writer *writer);
void fd_detail_items_free(struct fd_detail_items *items);

#endif
