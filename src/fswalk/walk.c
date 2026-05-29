#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "walk_internal.h"

static int bx_walk_recursive(DIR *dir,
                             const struct bx_walk_ctx *ctx,
                             int depth,
                             const struct bx_walk_ancestor *ancestors);
static int bx_walk_process_directory_root(DIR *opened_dir,
                                          const struct stat *st,
                                          struct bx_walk_ctx *ctx,
                                          struct bx_walk_path_buf *path_buf);
static int bx_walk_status_from_action(const struct bx_walk_ctx *ctx,
                                      enum bx_walk_action action,
                                      int *status_out);
static int bx_walk_recursive_visit_entry(const char *name,
                                         unsigned char d_type,
                                         int parent_dirfd,
                                         const struct bx_walk_ctx *ctx,
                                         int depth,
                                         const struct bx_walk_ancestor *ancestors,
                                         uint64_t *child_recursive_ns);

struct bx_walk_recursive_dirent_iter {
    int dirfd;
    const struct bx_walk_ctx *ctx;
    int depth;
    const struct bx_walk_ancestor *ancestors;
    uint64_t entries_seen;
    uint64_t *child_recursive_ns;
    int status;
};

struct bx_walk_path_buf {
    char *data;
    size_t len;
    size_t cap;
};

struct bx_walk_entry_path_state {
    struct bx_walk_path_buf *path_buf;
    const struct bx_walk_counter_ops *counter_ops;
    const char *name;
    size_t prev_len;
    bool appended;
};

static uint64_t bx_walk_monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void bx_walk_note_elapsed_ns(const struct bx_walk_counter_ops *counter_ops,
                                    enum bx_walk_counter counter,
                                    uint64_t start_ns) {
    if (!counter_ops || start_ns == 0u)
        return;
    uint64_t end_ns = bx_walk_monotonic_ns();
    if (end_ns >= start_ns)
        bx_walk_note_counter(counter_ops, counter, end_ns - start_ns);
}

static void bx_walk_add_u64_saturated(uint64_t *total, uint64_t value) {
    if (!total)
        return;
    if (UINT64_MAX - *total < value)
        *total = UINT64_MAX;
    else
        *total += value;
}

static void bx_walk_add_elapsed_ns(uint64_t *total, uint64_t start_ns) {
    if (!total || start_ns == 0u)
        return;
    uint64_t end_ns = bx_walk_monotonic_ns();
    if (end_ns >= start_ns)
        bx_walk_add_u64_saturated(total, end_ns - start_ns);
}

static void bx_walk_note_directory_bucket(const struct bx_walk_ctx *ctx,
                                          uint64_t entries,
                                          uint64_t elapsed_ns) {
    enum bx_walk_counter dirs_counter;
    enum bx_walk_counter entries_counter;
    enum bx_walk_counter ns_counter;

    if (!ctx || !ctx->opts || !ctx->opts->counter_ops)
        return;

    if (entries <= 8u) {
        dirs_counter = BX_WALK_COUNTER_DIR_BUCKET_TINY_DIRS;
        entries_counter = BX_WALK_COUNTER_DIR_BUCKET_TINY_ENTRIES;
        ns_counter = BX_WALK_COUNTER_DIR_BUCKET_TINY_NS;
    } else if (entries <= 64u) {
        dirs_counter = BX_WALK_COUNTER_DIR_BUCKET_SMALL_DIRS;
        entries_counter = BX_WALK_COUNTER_DIR_BUCKET_SMALL_ENTRIES;
        ns_counter = BX_WALK_COUNTER_DIR_BUCKET_SMALL_NS;
    } else if (entries <= 512u) {
        dirs_counter = BX_WALK_COUNTER_DIR_BUCKET_MEDIUM_DIRS;
        entries_counter = BX_WALK_COUNTER_DIR_BUCKET_MEDIUM_ENTRIES;
        ns_counter = BX_WALK_COUNTER_DIR_BUCKET_MEDIUM_NS;
    } else {
        dirs_counter = BX_WALK_COUNTER_DIR_BUCKET_HUGE_DIRS;
        entries_counter = BX_WALK_COUNTER_DIR_BUCKET_HUGE_ENTRIES;
        ns_counter = BX_WALK_COUNTER_DIR_BUCKET_HUGE_NS;
    }

    bx_walk_ctx_note_counter(ctx, dirs_counter, 1u);
    bx_walk_ctx_note_counter(ctx, entries_counter, entries);
    bx_walk_ctx_note_counter(ctx, ns_counter, elapsed_ns);
}

static bool bx_walk_path_buf_reserve(struct bx_walk_path_buf *buf,
                                     size_t needed,
                                     const struct bx_walk_counter_ops *counter_ops,
                                     int *err_out) {
    char *old_data;

    if (err_out)
        *err_out = 0;
    if (!buf) {
        if (err_out)
            *err_out = EINVAL;
        return false;
    }
    if (buf->cap >= needed)
        return true;

    size_t new_cap = buf->cap == 0u ? 64u : buf->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u) {
            if (err_out)
                *err_out = ENAMETOOLONG;
            return false;
        }
        new_cap *= 2u;
    }

    old_data = buf->data;
    char *tmp = realloc(buf->data, new_cap);
    if (!tmp) {
        if (err_out)
            *err_out = ENOMEM;
        return false;
    }
    bx_walk_note_counter(counter_ops, BX_WALK_COUNTER_PATH_ALLOCS, 1u);
    if (old_data && buf->len > 0u && tmp != old_data)
        bx_walk_note_counter(counter_ops, BX_WALK_COUNTER_PATH_COPIES_BEFORE_MATCH, 1u);
    buf->data = tmp;
    buf->cap = new_cap;
    return true;
}

static bool bx_walk_path_buf_init(struct bx_walk_path_buf *buf,
                                  const char *root,
                                  const struct bx_walk_counter_ops *counter_ops,
                                  int *err_out) {
    if (err_out)
        *err_out = 0;
    if (!buf || !root) {
        if (err_out)
            *err_out = EINVAL;
        return false;
    }

    memset(buf, 0, sizeof(*buf));
    size_t root_len = strlen(root);
    size_t initial_needed = root_len + 1u;
    if (root_len <= SIZE_MAX - 257u)
        initial_needed = root_len + 257u;
    if (!bx_walk_path_buf_reserve(buf, initial_needed, counter_ops, err_out))
        return false;
    memcpy(buf->data, root, root_len + 1u);
    buf->len = root_len;
    return true;
}

static void bx_walk_path_buf_free(struct bx_walk_path_buf *buf) {
    if (!buf)
        return;
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

static char *bx_walk_path_buf_push_name(struct bx_walk_path_buf *buf,
                                        const char *name,
                                        const struct bx_walk_counter_ops *counter_ops,
                                        int *err_out) {
    if (err_out)
        *err_out = 0;
    if (!buf || !buf->data || !name) {
        if (err_out)
            *err_out = EINVAL;
        return NULL;
    }

    size_t name_len = strlen(name);
    bx_walk_note_counter(counter_ops, BX_WALK_COUNTER_PATH_JOIN_CALLS, 1u);
    bx_walk_note_counter(counter_ops, BX_WALK_COUNTER_PATH_PUSH_CALLS, 1u);
    if (buf->len > SIZE_MAX - name_len - 2u) {
        if (err_out)
            *err_out = ENAMETOOLONG;
        return NULL;
    }

    size_t needed = buf->len + 1u + name_len + 1u;
    if (!bx_walk_path_buf_reserve(buf, needed, counter_ops, err_out))
        return NULL;

    buf->data[buf->len] = '/';
    memcpy(buf->data + buf->len + 1u, name, name_len);
    buf->len += 1u + name_len;
    buf->data[buf->len] = '\0';
    return buf->data;
}

static void bx_walk_path_buf_pop_to_len(struct bx_walk_path_buf *buf, size_t len) {
    if (!buf || !buf->data || len > buf->len)
        return;
    buf->len = len;
    buf->data[len] = '\0';
}

static char *bx_walk_entry_path_ensure(struct bx_walk_entry_path_state *state,
                                       const struct bx_walk_ctx *ctx,
                                       int *join_err) {
    if (join_err)
        *join_err = 0;
    if (!state || !ctx)
        return NULL;
    if (state->appended)
        return state->path_buf ? state->path_buf->data : NULL;

    uint64_t start_ns = state->counter_ops ? bx_walk_monotonic_ns() : 0u;
    char *full = bx_walk_path_buf_push_name(state->path_buf, state->name,
                                            ctx->opts->counter_ops, join_err);
    bx_walk_note_elapsed_ns(state->counter_ops, BX_WALK_COUNTER_PATH_PUSH_NS, start_ns);
    if (full)
        state->appended = true;
    return full;
}

static void bx_walk_entry_path_release(struct bx_walk_entry_path_state *state) {
    if (!state || !state->appended)
        return;
    bx_walk_note_counter(state->counter_ops, BX_WALK_COUNTER_PATH_POP_CALLS, 1u);
    uint64_t start_ns = state->counter_ops ? bx_walk_monotonic_ns() : 0u;
    bx_walk_path_buf_pop_to_len(state->path_buf, state->prev_len);
    bx_walk_note_elapsed_ns(state->counter_ops, BX_WALK_COUNTER_PATH_POP_NS, start_ns);
    state->appended = false;
}

static int bx_walk_open_directory_path(const char *dirpath,
                                       const struct bx_walk_ctx *ctx,
                                       DIR **dir_out,
                                       int *status_out) {
    DIR *dir = opendir(dirpath);
    if (dir) {
        *dir_out = dir;
        return 1;
    }

    if (errno == EACCES && ctx->opts->suppress_eacces) {
        if (ctx->opts->report_eacces)
            bx_walk_report_error(ctx->opts, dirpath, errno);
        *dir_out = NULL;
        return 0;
    }

    enum bx_walk_action action = bx_walk_handle_error(ctx, dirpath, errno);
    bx_walk_status_from_action(ctx, action, status_out);
    *dir_out = NULL;
    return status_out && *status_out != 0 ? -1 : 0;
}

static bool bx_walk_should_descend_child_directory(const struct bx_walk_entry *entry,
                                                   const struct bx_walk_opts *opts) {
    if (!entry || !opts || !entry->is_dir || entry->prune)
        return false;
    if (entry->is_symlink)
        return opts->follow_symlinks;
    return true;
}

static bool bx_walk_should_descend_root_directory(const struct bx_walk_entry *entry,
                                                  const struct bx_walk_opts *opts) {
    if (!entry || !opts || !entry->is_dir || entry->prune)
        return false;
    if (entry->is_symlink)
        return opts->follow_root_symlink;
    return true;
}

static int bx_walk_open_directory_child(int parent_dirfd,
                                        const char *name,
                                        struct bx_walk_entry_path_state *path_state,
                                        const struct bx_walk_ctx *ctx,
                                        DIR **dir_out,
                                        int *status_out) {
    const char *path = ctx && ctx->path_buf ? ctx->path_buf->data : NULL;
    /*
     * openat()/fdopendir() follow directory symlinks. The caller must decide
     * whether a symlink-backed directory is descendable before we reach this
     * open boundary.
     */
    if (parent_dirfd < 0 || !name) {
        int join_err = 0;
        char *full = bx_walk_entry_path_ensure(path_state, ctx, &join_err);
        if (full)
            path = full;
        return bx_walk_open_directory_path(path, ctx, dir_out, status_out);
    }

    bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_OPENAT_CALLS, 1u);
    int fd = openat(parent_dirfd, name, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd < 0) {
        int join_err = 0;
        char *full = bx_walk_entry_path_ensure(path_state, ctx, &join_err);
        if (full)
            path = full;
        if (errno == EACCES && ctx->opts->suppress_eacces) {
            if (ctx->opts->report_eacces)
                bx_walk_report_error(ctx->opts, path, errno);
            *dir_out = NULL;
            return 0;
        }
        enum bx_walk_action action = bx_walk_handle_error(ctx, path, errno);
        bx_walk_status_from_action(ctx, action, status_out);
        *dir_out = NULL;
        return status_out && *status_out != 0 ? -1 : 0;
    }

    DIR *dir = fdopendir(fd);
    if (dir) {
        *dir_out = dir;
        return 1;
    }

    int saved_errno = errno != 0 ? errno : EIO;
    close(fd);
    errno = saved_errno;
    int join_err = 0;
    char *full = bx_walk_entry_path_ensure(path_state, ctx, &join_err);
    if (full)
        path = full;
    if (saved_errno == EACCES && ctx->opts->suppress_eacces) {
        if (ctx->opts->report_eacces)
            bx_walk_report_error(ctx->opts, path, saved_errno);
        *dir_out = NULL;
        return 0;
    }
    enum bx_walk_action action = bx_walk_handle_error(ctx, path, saved_errno);
    bx_walk_status_from_action(ctx, action, status_out);
    *dir_out = NULL;
    return status_out && *status_out != 0 ? -1 : 0;
}

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

static int bx_walk_recursive_iterate_dirent(const char *name,
                                            unsigned char d_type,
                                            void *user) {
    struct bx_walk_recursive_dirent_iter *state = user;
    if (!state)
        return -1;

    state->entries_seen++;
    bx_walk_ctx_note_counter(state->ctx, BX_WALK_COUNTER_DIRENTS_SEEN, 1u);
    if (d_type == DT_UNKNOWN)
        bx_walk_ctx_note_counter(state->ctx, BX_WALK_COUNTER_UNKNOWN_DTYPE_SEEN, 1u);
    if (bx_walk_recursive_visit_entry(name, d_type,
                                      state->dirfd,
                                      state->ctx, state->depth, state->ancestors,
                                      state->child_recursive_ns) != 0) {
        state->status = -1;
    }
    if (bx_walk_should_stop(state->ctx->opts))
        return 1;
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

    if (!entry->metadata_loaded &&
        !bx_walk_entry_load_metadata_for(entry, BX_WALK_METADATA_REASON_TRAVERSAL_POLICY))
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

static int bx_walk_recursive_visit_entry(const char *name,
                                         unsigned char d_type,
                                         int parent_dirfd,
                                         const struct bx_walk_ctx *ctx,
                                         int depth,
                                         const struct bx_walk_ancestor *ancestors,
                                         uint64_t *child_recursive_ns) {
    if (bx_walk_should_stop(ctx->opts))
        return 0;

    if (ctx->opts->max_depth >= 0 && depth + 1 > ctx->opts->max_depth)
        return 0;

    struct bx_walk_entry_path_state path_state = {
        .path_buf = ctx->path_buf,
        .counter_ops = ctx->opts->counter_ops,
        .name = name,
        .prev_len = ctx->path_buf ? ctx->path_buf->len : 0u,
    };

    int status = 0;
    bool entry_was_symlink = false;
    struct bx_walk_entry entry;
    int fill_err = bx_walk_entry_fill_from_dirent(&entry, NULL, name, d_type,
                                                  parent_dirfd,
                                                  ctx->opts->follow_symlinks,
                                                  depth + 1,
                                                  ctx->opts->counter_ops,
                                                  &entry_was_symlink);
    if (fill_err != 0) {
        int join_err = 0;
        char *full = bx_walk_entry_path_ensure(&path_state, ctx, &join_err);
        enum bx_walk_action action = bx_walk_handle_error(ctx,
                                                          full ? full : ctx->path_buf->data,
                                                          fill_err);
        bx_walk_status_from_action(ctx, action, &status);
        bx_walk_entry_path_release(&path_state);
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
        int join_err = 0;
        char *full = bx_walk_entry_path_ensure(&path_state, ctx, &join_err);
        enum bx_walk_action action = bx_walk_handle_error(ctx,
                                                          full ? full : ctx->path_buf->data,
                                                          dir_err);
        bx_walk_status_from_action(ctx, action, &status);
        bx_walk_entry_path_release(&path_state);
        return status;
    }

    if (repeated_dir) {
        int join_err = 0;
        char *full = bx_walk_entry_path_ensure(&path_state, ctx, &join_err);
        if (ctx->opts->cycle_report != BX_WALK_CYCLE_IGNORE)
            bx_walk_report_loop(ctx->opts, full ? full : ctx->path_buf->data);
        if (ctx->opts->cycle_report == BX_WALK_CYCLE_ERROR)
            status = -1;
        bx_walk_entry_path_release(&path_state);
        return status;
    }

    if (crosses_filesystem) {
        bx_walk_entry_path_release(&path_state);
        return 0;
    }

    int join_err = 0;
    char *full = bx_walk_entry_path_ensure(&path_state, ctx, &join_err);
    if (!full) {
        enum bx_walk_action action = bx_walk_handle_error(ctx, ctx->path_buf->data,
                                                          join_err != 0 ? join_err : ENOMEM);
        bx_walk_status_from_action(ctx, action, &status);
        bx_walk_entry_path_release(&path_state);
        return status;
    }
    entry.path = full;

    if (!ctx->opts->post_order) {
        enum bx_walk_action action = bx_walk_apply_visit_action(ctx, &entry, &status);
        if (action == BX_WALK_STOP || action == BX_WALK_ERROR) {
            bx_walk_entry_path_release(&path_state);
            return status;
        }
    }

    if (!bx_walk_should_stop(ctx->opts) &&
        bx_walk_should_descend_child_directory(&entry, ctx->opts)) {
        struct bx_walk_ancestor next = {
            .dev = entry.dev,
            .ino = entry.inode,
            .path = NULL,
            .parent = ancestors,
        };
        DIR *child_dir = NULL;
        int open_rc = bx_walk_open_directory_child(parent_dirfd, name, &path_state,
                                                   ctx, &child_dir, &status);
        if (open_rc < 0) {
            status = -1;
        } else if (open_rc > 0) {
            uint64_t child_start_ns = child_recursive_ns ? bx_walk_monotonic_ns() : 0u;
            if (bx_walk_recursive(child_dir, ctx, depth + 1, &next) != 0)
                status = -1;
            bx_walk_add_elapsed_ns(child_recursive_ns, child_start_ns);
        }
    }

    if (!bx_walk_should_stop(ctx->opts) && ctx->opts->post_order) {
        enum bx_walk_action action = bx_walk_apply_visit_action(ctx, &entry, &status);
        if (action == BX_WALK_STOP || action == BX_WALK_ERROR) {
            bx_walk_entry_path_release(&path_state);
            return status;
        }
    }

    bx_walk_entry_path_release(&path_state);
    return status;
}

static int bx_walk_recursive(DIR *dir,
                             const struct bx_walk_ctx *ctx,
                             int depth,
                             const struct bx_walk_ancestor *ancestors) {
    if (bx_walk_should_stop(ctx->opts)) {
        closedir(dir);
        return 0;
    }
    if (ctx->opts->max_depth >= 0 && depth > ctx->opts->max_depth) {
        closedir(dir);
        return 0;
    }

    int status = 0;
    uint64_t dir_start_ns = ctx->opts->counter_ops ? bx_walk_monotonic_ns() : 0u;
    uint64_t child_recursive_ns = 0u;
    uint64_t *child_recursive_ns_out = dir_start_ns != 0u ? &child_recursive_ns : NULL;
    uint64_t dir_entries_seen = 0u;
    uint64_t dir_elapsed_ns = 0u;
    if (ctx->opts->sort_entries || ctx->opts->reverse_sort) {
        struct bx_walk_dirent_list dirents = {0};
        int dirent_err = 0;
        if (bx_walk_dirent_list_read_sorted(dir, &dirents, &dirent_err,
                                            ctx->opts->counter_ops) != 0) {
            enum bx_walk_action action = bx_walk_handle_error(ctx, ctx->path_buf->data,
                                                              dirent_err != 0 ? dirent_err : EIO);
            bx_walk_dirent_list_free(&dirents);
            bx_walk_status_from_action(ctx, action, &status);
            goto out;
        }

        dir_entries_seen = dirents.len;
        for (size_t iter_index = 0; iter_index < dirents.len; iter_index++) {
            size_t dirent_index = ctx->opts->reverse_sort ? (dirents.len - 1u - iter_index)
                                                          : iter_index;
            const struct bx_walk_dirent_item *item = &dirents.items[dirent_index];
            bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_DIRENTS_SEEN, 1u);
            if (item->d_type == DT_UNKNOWN)
                bx_walk_ctx_note_counter(ctx, BX_WALK_COUNTER_UNKNOWN_DTYPE_SEEN, 1u);
            if (bx_walk_recursive_visit_entry(item->name, item->d_type,
                                              dirfd(dir),
                                              ctx, depth, ancestors,
                                              child_recursive_ns_out) != 0)
                status = -1;
            if (bx_walk_should_stop(ctx->opts))
                break;
        }
        bx_walk_dirent_list_free(&dirents);
    } else {
        struct bx_walk_recursive_dirent_iter iter = {
            .dirfd = dirfd(dir),
            .ctx = ctx,
            .depth = depth,
            .ancestors = ancestors,
            .child_recursive_ns = child_recursive_ns_out,
        };
        int dirent_err = 0;
        int dirent_rc = bx_walk_dirent_iterate(dir, bx_walk_recursive_iterate_dirent,
                                               &iter, &dirent_err,
                                               ctx->opts->counter_ops);
        status = iter.status;
        dir_entries_seen = iter.entries_seen;

        if (!bx_walk_should_stop(ctx->opts) && dirent_rc < 0 && dirent_err != 0) {
            enum bx_walk_action action = bx_walk_handle_error(ctx, ctx->path_buf->data, dirent_err);
            bx_walk_status_from_action(ctx, action, &status);
        }
    }

out:
    if (dir_start_ns != 0u) {
        uint64_t dir_end_ns = bx_walk_monotonic_ns();
        if (dir_end_ns >= dir_start_ns)
            dir_elapsed_ns = dir_end_ns - dir_start_ns;
        if (dir_elapsed_ns >= child_recursive_ns)
            dir_elapsed_ns -= child_recursive_ns;
        else
            dir_elapsed_ns = 0u;
    }
    bx_walk_note_directory_bucket(ctx, dir_entries_seen, dir_elapsed_ns);
    closedir(dir);
    return status;
}

static int bx_walk_process_directory_root(DIR *opened_dir,
                                          const struct stat *st,
                                          struct bx_walk_ctx *ctx,
                                          struct bx_walk_path_buf *path_buf) {
    struct bx_walk_entry entry = {
        .path = path_buf->data,
        .follow_metadata = ctx->opts->follow_root_symlink,
        .depth = 0,
        .counter_ops = ctx->opts->counter_ops,
    };
    bx_walk_entry_fill_from_stat(&entry, st);
    bx_walk_note_counter(ctx->opts->counter_ops, BX_WALK_COUNTER_DIRS_SEEN, 1u);

    int status = 0;
    if (!ctx->opts->post_order) {
        enum bx_walk_action action = bx_walk_apply_visit_action(ctx, &entry, &status);
        if (action == BX_WALK_STOP || action == BX_WALK_ERROR) {
            if (opened_dir)
                closedir(opened_dir);
            return status;
        }
    }

    if (!bx_walk_should_stop(ctx->opts) &&
        bx_walk_should_descend_root_directory(&entry, ctx->opts)) {
        DIR *root_dir = opened_dir;
        struct bx_walk_ancestor root_ancestor = {
            .dev = st->st_dev,
            .ino = st->st_ino,
            .path = NULL,
            .parent = NULL,
        };
        int open_rc = root_dir ? 1 : bx_walk_open_directory_path(path_buf->data,
                                                                 ctx,
                                                                 &root_dir,
                                                                 &status);
        if (open_rc < 0) {
            status = -1;
        } else if (open_rc > 0 && bx_walk_recursive(root_dir, ctx, 0, &root_ancestor) != 0) {
            status = -1;
        }
        opened_dir = NULL;
    }

    if (opened_dir)
        closedir(opened_dir);

    if (!bx_walk_should_stop(ctx->opts) && ctx->opts->post_order) {
        enum bx_walk_action action = bx_walk_apply_visit_action(ctx, &entry, &status);
        (void)action;
    }

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
    bx_walk_note_stat_call_for_reason(
        opts->counter_ops,
        opts->follow_root_symlink ? BX_WALK_COUNTER_STAT_CALLS : BX_WALK_COUNTER_LSTAT_CALLS,
        BX_WALK_COUNTER_STAT_REASON_EXPLICIT_OPERAND);
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
    struct bx_walk_path_buf path_buf;
    int path_err = 0;
    if (!bx_walk_path_buf_init(&path_buf, root, opts->counter_ops, &path_err)) {
        enum bx_walk_action action = bx_walk_handle_error(&ctx, root, path_err != 0 ? path_err : ENOMEM);
        int status = 0;
        bx_walk_status_from_action(&ctx, action, &status);
        return status;
    }
    ctx.path_buf = &path_buf;

    if (S_ISDIR(st.st_mode)) {
        int status = bx_walk_process_directory_root(NULL, &st, &ctx, &path_buf);
        bx_walk_path_buf_free(&path_buf);
        return status;
    }

    struct bx_walk_entry entry = {
        .path = path_buf.data,
        .follow_metadata = opts->follow_root_symlink,
        .depth = 0,
        .counter_ops = opts->counter_ops,
    };
    bx_walk_entry_fill_from_stat(&entry, &st);
    if (S_ISLNK(st.st_mode))
        bx_walk_note_counter(opts->counter_ops, BX_WALK_COUNTER_SYMLINKS_SEEN, 1u);
    else
        bx_walk_note_counter(opts->counter_ops, BX_WALK_COUNTER_FILES_SEEN, 1u);
    int status = 0;
    (void)bx_walk_apply_visit_action(&ctx, &entry, &status);
    bx_walk_path_buf_free(&path_buf);
    return status;
}

int bx_walk_opened_dir(const char *root,
                       DIR *root_dir,
                       const struct bx_walk_opts *opts,
                       const struct bx_walk_ops *ops,
                       void *user) {
    if (!root || !root_dir || !opts || !ops || !ops->visit) {
        if (root_dir)
            closedir(root_dir);
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    bx_walk_note_stat_call_for_reason(opts->counter_ops,
                                      BX_WALK_COUNTER_FSTAT_CALLS,
                                      BX_WALK_COUNTER_STAT_REASON_EXPLICIT_OPERAND);
    if (fstat(dirfd(root_dir), &st) != 0) {
        int err = errno;
        struct bx_walk_ctx ctx = {.opts = opts, .ops = ops, .user = user, .root_device = 0};
        enum bx_walk_action action = bx_walk_handle_error(&ctx, root, err);
        int status = 0;
        bx_walk_status_from_action(&ctx, action, &status);
        closedir(root_dir);
        errno = err;
        return status;
    }

    struct bx_walk_ctx ctx = {
        .opts = opts,
        .ops = ops,
        .user = user,
        .root_device = st.st_dev,
    };
    struct bx_walk_path_buf path_buf;
    int path_err = 0;
    if (!bx_walk_path_buf_init(&path_buf, root, opts->counter_ops, &path_err)) {
        enum bx_walk_action action =
            bx_walk_handle_error(&ctx, root, path_err != 0 ? path_err : ENOMEM);
        int status = 0;
        bx_walk_status_from_action(&ctx, action, &status);
        closedir(root_dir);
        return status;
    }
    ctx.path_buf = &path_buf;

    int status = bx_walk_process_directory_root(root_dir, &st, &ctx, &path_buf);
    bx_walk_path_buf_free(&path_buf);
    return status;
}
