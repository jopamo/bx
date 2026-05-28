#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "walk_internal.h"

#define BX_WALK_STACK_PATH_CAP 1024u

static int bx_walk_recursive(const char *dirpath,
                             const struct bx_walk_ctx *ctx,
                             int depth,
                             const struct bx_walk_ancestor *ancestors);

static int bx_walk_status_from_action(const struct bx_walk_ctx *ctx,
                                      enum bx_walk_action action,
                                      int *status_out) {
    switch (action) {
    case BX_WALK_CONTINUE:
    case BX_WALK_PRUNE:
        return 0;
    case BX_WALK_STOP:
        if (ctx->opts->stop)
            *ctx->opts->stop = true;
        return 0;
    case BX_WALK_ERROR:
        if (status_out)
            *status_out = -1;
        return -1;
    }
    return -1;
}

static int bx_walk_fill_entry(char *path,
                              unsigned char d_type,
                              const struct bx_walk_ctx *ctx,
                              int depth,
                              struct bx_walk_entry *entry,
                              bool *entry_was_symlink) {
    memset(entry, 0, sizeof(*entry));
    entry->path = path;
    entry->d_type = d_type;
    entry->d_type_known = d_type != DT_UNKNOWN;
    entry->follow_metadata = ctx->opts->follow_symlinks;
    entry->depth = depth;
    entry->counter_ops = ctx->opts->counter_ops;
    *entry_was_symlink = false;

    struct stat st;
    struct stat lst;

    if (!ctx->opts->follow_symlinks) {
        if (d_type == DT_DIR) {
            entry->is_dir = true;
            return 0;
        }
        if (d_type == DT_LNK) {
            entry->is_symlink = true;
            return 0;
        }
        if (d_type != DT_UNKNOWN) {
            entry->is_dir = false;
            return 0;
        }
        bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_LSTAT_CALLS, 1u);
        if (lstat(path, &st) != 0)
            return errno;
        bx_walk_entry_fill_from_stat(entry, &st);
        entry->is_symlink = S_ISLNK(st.st_mode);
        return 0;
    }

    if (d_type == DT_DIR) {
        entry->is_dir = true;
        return 0;
    }
    if (d_type != DT_LNK && d_type != DT_UNKNOWN) {
        entry->is_dir = false;
        return 0;
    }

    bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_LSTAT_CALLS, 1u);
    if (lstat(path, &lst) != 0)
        return errno;

    *entry_was_symlink = S_ISLNK(lst.st_mode);
    entry->is_symlink = *entry_was_symlink;
    if (!*entry_was_symlink) {
        if (stat(path, &st) != 0)
            return errno;
        bx_walk_entry_fill_from_stat(entry, &st);
        return 0;
    }

    if (stat(path, &st) == 0) {
        bx_walk_entry_fill_from_stat(entry, &st);
    } else {
        bx_walk_entry_fill_from_stat(entry, &lst);
    }
    return 0;
}

static int bx_walk_prepare_directory(struct bx_walk_entry *entry,
                                     const struct bx_walk_ctx *ctx,
                                     const struct bx_walk_ancestor *ancestors,
                                     bool entry_was_symlink,
                                     bool *crosses_filesystem,
                                     bool *repeated_dir) {
    *crosses_filesystem = false;
    *repeated_dir = false;

    if (!entry->is_dir)
        return 0;

    if (!ctx->opts->stay_on_filesystem && ctx->opts->cycle_mode == BX_WALK_CYCLE_NONE)
        return 0;

    if (!entry->metadata_loaded && !bx_walk_entry_load_metadata(entry))
        return errno != 0 ? errno : ENOENT;

    *crosses_filesystem = ctx->opts->stay_on_filesystem && entry->dev != ctx->root_device;
    if (*crosses_filesystem)
        return 0;

    switch (ctx->opts->cycle_mode) {
    case BX_WALK_CYCLE_NONE:
        return 0;
    case BX_WALK_CYCLE_DIR_REPEAT:
        *repeated_dir = bx_walk_ancestor_contains(ancestors, entry->dev, entry->inode);
        return 0;
    case BX_WALK_CYCLE_SYMLINK_REPEAT:
        *repeated_dir = entry_was_symlink &&
                        bx_walk_ancestor_contains(ancestors, entry->dev, entry->inode);
        return 0;
    }

    return 0;
}

static int bx_walk_recursive_visit_entry(const char *dirpath,
                                         const char *name,
                                         unsigned char d_type,
                                         const struct bx_walk_ctx *ctx,
                                         int depth,
                                         const struct bx_walk_ancestor *ancestors) {
    if (bx_walk_should_stop(ctx->opts))
        return 0;

    if (ctx->opts->max_depth >= 0 && depth + 1 > ctx->opts->max_depth)
        return 0;

    int join_err = 0;
    char stack_full[BX_WALK_STACK_PATH_CAP];
    char *full = NULL;
    bool full_is_heap = false;
    size_t dir_len = strlen(dirpath);
    size_t name_len = strlen(name);
    bool likely_dir = d_type == DT_DIR;

    bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_PATH_JOIN_CALLS, 1u);
    if (dir_len > SIZE_MAX - name_len - 2u) {
        join_err = ENAMETOOLONG;
    } else {
        size_t full_len = dir_len + 1u + name_len + 1u;

        if (!likely_dir && full_len <= sizeof(stack_full)) {
            full = stack_full;
            memcpy(full, dirpath, dir_len);
            full[dir_len] = '/';
            memcpy(full + dir_len + 1u, name, name_len);
            full[full_len - 1u] = '\0';
        } else {
            full = bx_walk_path_join(dirpath, name, &join_err, ctx->opts->counter_ops);
            full_is_heap = full != NULL;
        }
    }
    if (!full) {
        enum bx_walk_action action = bx_walk_handle_error(ctx, dirpath,
                                                          join_err != 0 ? join_err : ENOMEM);
        int status = 0;
        bx_walk_status_from_action(ctx, action, &status);
        return status;
    }

    int status = 0;
    bool entry_was_symlink = false;
    struct bx_walk_entry entry;
    int fill_err = bx_walk_fill_entry(full, d_type, ctx, depth + 1, &entry, &entry_was_symlink);
    if (fill_err != 0) {
        enum bx_walk_action action = bx_walk_handle_error(ctx, full, fill_err);
        bx_walk_status_from_action(ctx, action, &status);
        if (full_is_heap)
            free(full);
        return status;
    }
    if (entry.is_symlink) {
        bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_SYMLINKS_SEEN, 1u);
    } else if (entry.is_dir) {
        bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_DIRS_SEEN, 1u);
    } else {
        bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_FILES_SEEN, 1u);
    }

    bool crosses_filesystem = false;
    bool repeated_dir = false;
    int dir_err = bx_walk_prepare_directory(&entry, ctx, ancestors, entry_was_symlink,
                                            &crosses_filesystem, &repeated_dir);
    if (dir_err != 0) {
        enum bx_walk_action action = bx_walk_handle_error(ctx, full, dir_err);
        bx_walk_status_from_action(ctx, action, &status);
        if (full_is_heap)
            free(full);
        return status;
    }

    if (repeated_dir) {
        if (ctx->opts->cycle_report != BX_WALK_CYCLE_IGNORE)
            bx_walk_report_loop(ctx->opts, full);
        if (ctx->opts->cycle_report == BX_WALK_CYCLE_ERROR)
            status = -1;
        if (full_is_heap)
            free(full);
        return status;
    }

    if (crosses_filesystem) {
        if (full_is_heap)
            free(full);
        return 0;
    }

    if (!ctx->opts->post_order) {
        enum bx_walk_action action = bx_walk_apply_visit_action(ctx, &entry, &status);
        if (action == BX_WALK_STOP || action == BX_WALK_ERROR) {
            if (full_is_heap)
                free(full);
            return status;
        }
    }

    if (!bx_walk_should_stop(ctx->opts) && entry.is_dir && !entry.prune) {
        if (!full_is_heap) {
            full = strdup(stack_full);
            if (!full) {
                enum bx_walk_action action = bx_walk_handle_error(ctx, stack_full, ENOMEM);
                bx_walk_status_from_action(ctx, action, &status);
                return status;
            }
            bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_PATH_ALLOCS, 1u);
            bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_PATH_COPIES_BEFORE_MATCH, 1u);
            full_is_heap = true;
            entry.path = full;
        }
        struct bx_walk_ancestor next = {
            .dev = entry.dev,
            .ino = entry.inode,
            .path = full,
            .parent = ancestors,
        };
        if (bx_walk_recursive(full, ctx, depth + 1, &next) != 0)
            status = -1;
    }

    if (!bx_walk_should_stop(ctx->opts) && ctx->opts->post_order) {
        enum bx_walk_action action = bx_walk_apply_visit_action(ctx, &entry, &status);
        if (action == BX_WALK_STOP || action == BX_WALK_ERROR) {
            if (full_is_heap)
                free(full);
            return status;
        }
    }

    if (full_is_heap)
        free(full);
    return status;
}

static int bx_walk_recursive(const char *dirpath,
                             const struct bx_walk_ctx *ctx,
                             int depth,
                             const struct bx_walk_ancestor *ancestors) {
    if (bx_walk_should_stop(ctx->opts))
        return 0;
    if (ctx->opts->max_depth >= 0 && depth > ctx->opts->max_depth)
        return 0;

    DIR *dir = opendir(dirpath);
    if (!dir) {
        if (errno == EACCES && ctx->opts->suppress_eacces) {
            if (ctx->opts->report_eacces)
                bx_walk_report_error(ctx->opts, dirpath, errno);
            return 0;
        }
        enum bx_walk_action action = bx_walk_handle_error(ctx, dirpath, errno);
        int status = 0;
        bx_walk_status_from_action(ctx, action, &status);
        return status;
    }

    int status = 0;
    if (ctx->opts->sort_entries || ctx->opts->reverse_sort) {
        struct bx_walk_dirent_list dirents = {0};
        int dirent_err = 0;
        if (bx_walk_dirent_list_read_sorted(dir, &dirents, &dirent_err) != 0) {
            enum bx_walk_action action = bx_walk_handle_error(ctx, dirpath,
                                                              dirent_err != 0 ? dirent_err : EIO);
            bx_walk_dirent_list_free(&dirents);
            closedir(dir);
            bx_walk_status_from_action(ctx, action, &status);
            return status;
        }

        for (size_t iter_index = 0; iter_index < dirents.len; iter_index++) {
            size_t dirent_index = ctx->opts->reverse_sort ? (dirents.len - 1u - iter_index)
                                                          : iter_index;
            const struct bx_walk_dirent_item *item = &dirents.items[dirent_index];
            bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_DIRENTS_SEEN, 1u);
            if (item->d_type == DT_UNKNOWN)
                bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_UNKNOWN_DTYPE_SEEN, 1u);
            if (bx_walk_recursive_visit_entry(dirpath, item->name, item->d_type,
                                              ctx, depth, ancestors) != 0)
                status = -1;
            if (bx_walk_should_stop(ctx->opts))
                break;
        }
        bx_walk_dirent_list_free(&dirents);
    } else {
        errno = 0;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_DIRENTS_SEEN, 1u);
            if (ent->d_type == DT_UNKNOWN)
                bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_UNKNOWN_DTYPE_SEEN, 1u);
            if (bx_walk_recursive_visit_entry(dirpath, ent->d_name, ent->d_type,
                                              ctx, depth, ancestors) != 0)
                status = -1;
            if (bx_walk_should_stop(ctx->opts))
                break;
            errno = 0;
        }

        if (!bx_walk_should_stop(ctx->opts) && errno != 0) {
            enum bx_walk_action action = bx_walk_handle_error(ctx, dirpath, errno);
            bx_walk_status_from_action(ctx, action, &status);
        }
    }

    closedir(dir);
    return status;
}

int bx_walk(const char *root,
            const struct bx_walk_opts *opts,
            const struct bx_walk_ops *ops,
            void *user) {
    if (!root || !opts || !ops || !ops->visit) {
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    if (!opts->follow_root_symlink)
        bx_walk_note_counter(opts->counter_ops, BX_WALK_COUNTER_LSTAT_CALLS, 1u);
    int root_stat_rc = opts->follow_root_symlink ? stat(root, &st) : lstat(root, &st);
    if (root_stat_rc != 0) {
        struct bx_walk_ctx ctx = {.opts = opts, .ops = ops, .user = user, .root_device = 0};
        enum bx_walk_action action = bx_walk_handle_error(&ctx, root, errno);
        int status = 0;
        bx_walk_status_from_action(&ctx, action, &status);
        return status;
    }

    struct bx_walk_ctx ctx = {
        .opts = opts,
        .ops = ops,
        .user = user,
        .root_device = st.st_dev,
    };

    if (S_ISDIR(st.st_mode)) {
        struct bx_walk_entry entry = {
            .path = strdup(root),
            .follow_metadata = opts->follow_root_symlink,
            .depth = 0,
            .counter_ops = opts->counter_ops,
        };
        if (!entry.path) {
            enum bx_walk_action action = bx_walk_handle_error(&ctx, root, ENOMEM);
            int status = 0;
            bx_walk_status_from_action(&ctx, action, &status);
            return status;
        }
        bx_walk_note_counter(opts->counter_ops, BX_WALK_COUNTER_PATH_ALLOCS, 1u);
        bx_walk_note_counter(opts->counter_ops, BX_WALK_COUNTER_PATH_COPIES_BEFORE_MATCH, 1u);
        bx_walk_entry_fill_from_stat(&entry, &st);
        bx_walk_note_counter(opts->counter_ops, BX_WALK_COUNTER_DIRS_SEEN, 1u);

        int status = 0;
        if (!opts->post_order) {
            enum bx_walk_action action = bx_walk_apply_visit_action(&ctx, &entry, &status);
            if (action == BX_WALK_STOP || action == BX_WALK_ERROR) {
                free(entry.path);
                return status;
            }
        }

        if (!bx_walk_should_stop(opts) && !entry.prune) {
            struct bx_walk_ancestor root_ancestor = {
                .dev = st.st_dev,
                .ino = st.st_ino,
                .path = root,
                .parent = NULL,
            };
            if (bx_walk_recursive(root, &ctx, 0, &root_ancestor) != 0)
                status = -1;
        }

        if (!bx_walk_should_stop(opts) && opts->post_order) {
            enum bx_walk_action action = bx_walk_apply_visit_action(&ctx, &entry, &status);
            (void)action;
        }

        free(entry.path);
        return status;
    }

    struct bx_walk_entry entry = {
        .path = strdup(root),
        .follow_metadata = opts->follow_root_symlink,
        .depth = 0,
        .counter_ops = opts->counter_ops,
    };
    if (!entry.path) {
        enum bx_walk_action action = bx_walk_handle_error(&ctx, root, ENOMEM);
        int status = 0;
        bx_walk_status_from_action(&ctx, action, &status);
        return status;
    }
    bx_walk_note_counter(opts->counter_ops, BX_WALK_COUNTER_PATH_ALLOCS, 1u);
    bx_walk_note_counter(opts->counter_ops, BX_WALK_COUNTER_PATH_COPIES_BEFORE_MATCH, 1u);
    bx_walk_entry_fill_from_stat(&entry, &st);
    if (S_ISLNK(st.st_mode))
        bx_walk_note_counter(opts->counter_ops, BX_WALK_COUNTER_SYMLINKS_SEEN, 1u);
    else
        bx_walk_note_counter(opts->counter_ops, BX_WALK_COUNTER_FILES_SEEN, 1u);
    int status = 0;
    (void)bx_walk_apply_visit_action(&ctx, &entry, &status);
    free(entry.path);
    return status;
}
