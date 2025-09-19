#ifndef BX_APPLETS_TEXT_FD_OUTPUT_H
#define BX_APPLETS_TEXT_FD_OUTPUT_H

#include <stdbool.h>
#include "fd_exec_render.h"
#include "fd_internal.h"
#include "search/walk.h"

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
    int count;
    int cap;
};

bool fd_print_match_output(const struct fd_render_ctx *ctx, const struct fd_opts *opts,
                           const char *path, bool is_dir);
bool fd_detail_items_append(struct fd_detail_items *items,
                            const struct fd_render_ctx *ctx,
                            struct walk_entry *entry);
int fd_detail_items_print(struct fd_detail_items *items);
void fd_detail_items_free(struct fd_detail_items *items);

#endif
