#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "copy_tree.h"
#include "copy_data.h"
#include "copy_metadata.h"
#include "update_policy.h"
#include "args_common.h"
#include "backup_ops.h"
#include "overwrite_ops.h"
#include "path_ops.h"
#include "same_file.h"
#include "stat_ops.h"
#include "fd_ops.h"
#include "bx/diag.h"
#include "bx/libbx.h"

char* realpath(const char* restrict path, char* restrict resolved_path);

static bool bx_copy_should_follow_source(const struct bx_copy_options* options, bool top_level, bool source_is_symlink) {
    if (!source_is_symlink) {
        return false;
    }

    switch (options->deref_mode) {
        case BX_DEREF_ALWAYS:
            return true;
        case BX_DEREF_NEVER:
            return false;
        case BX_DEREF_COMMAND_LINE:
            return top_level;
        case BX_DEREF_DEFAULT:
            return !options->recursive;
    }

    return false;
}

static mode_t bx_copy_regular_file_create_mode(const struct bx_copy_context* ctx, const struct stat* src_stat) {
    if (ctx->options->mode_policy == BX_MODE_POLICY_NO_PRESERVE) {
        return 0666u;
    }
    return src_stat->st_mode & 0777u;
}

static char* bx_copy_parent_dir_dup(const char* path) {
    return bx_path_parent_dir_dup(path);
}

static bool bx_copy_relative_symlink_stays_in_cwd(const char* dest_path) {
    char* parent = bx_copy_parent_dir_dup(dest_path);
    char* parent_real = realpath(parent, NULL);
    bool same_directory = true;

    free(parent);

    if (parent_real == NULL) {
        return true;
    }

    char* cwd_real = realpath(".", NULL);
    if (cwd_real != NULL) {
        same_directory = strcmp(parent_real, cwd_real) == 0;
        free(cwd_real);
    }

    free(parent_real);
    return same_directory;
}

static const struct stat* bx_copy_overwrite_dest_stat(const struct bx_copy_context* ctx, const struct bx_dest_state* dest_state) {
    if (dest_state->exists_stat) {
        return &dest_state->st;
    }
    if (dest_state->dangling_symlink && bx_args_backup_mode_enabled(ctx->backup_params.mode)) {
        return &dest_state->lst;
    }
    return NULL;
}

static mode_t bx_copy_directory_create_mode(const struct bx_copy_context* ctx, const struct stat* src_stat, mode_t* final_mode_out, bool* restore_mode_out) {
    mode_t source_mode = src_stat->st_mode & 0777u;

    if (ctx->options->mode_policy == BX_MODE_POLICY_PRESERVE) {
        *final_mode_out = 0;
        *restore_mode_out = false;
        return source_mode | S_IRWXU;
    }

    if (ctx->options->mode_policy == BX_MODE_POLICY_NO_PRESERVE) {
        *final_mode_out = 0777u & ~ctx->umask_value;
    }
    else {
        *final_mode_out = source_mode & ~ctx->umask_value;
    }
    *restore_mode_out = (*final_mode_out | S_IRWXU) != *final_mode_out;
    return *final_mode_out | S_IRWXU;
}

static bool bx_copy_unlink_existing_file(const struct bx_copy_context* ctx, const char* dest_path) {
    if (unlink(dest_path) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    return true;
}

static bool bx_copy_reject_directory_dest(const struct bx_copy_context* ctx, const char* source_path, const char* dest_path, const struct bx_dest_state* dest_state) {
    if (!dest_state->exists_lstat || !S_ISDIR(dest_state->lst.st_mode)) {
        return true;
    }

    bx_diag(ctx->diag, "cannot overwrite directory '%s' with non-directory '%s'", dest_path, source_path);
    return false;
}

static enum bx_backup_create_result bx_copy_backup_same_file_copy(struct bx_copy_context* ctx, const char* src_path, const char* dest_path) {
    char* backup_file = NULL;
    enum bx_backup_create_result result = bx_backup_create_copy(dest_path, &ctx->backup_params, ctx->diag, &backup_file);

    if (result == BX_BACKUP_CREATE_CREATED) {
        bx_info(ctx->diag, "'%s' -> '%s'", src_path, backup_file);
        free(backup_file);
    }
    return result;
}

static bool bx_copy_parent_dir_stat(const char* path, struct stat* parent_stat_out) {
    char* parent_path = bx_path_parent_dir_stripped_dup(path);

    bool ok = stat(parent_path, parent_stat_out) == 0;
    free(parent_path);
    return ok;
}

static bool bx_copy_paths_name_same_directory_entry(const char* src_path, const char* dest_path) {
    bool same_entry = true;
    char* src_base = bx_path_basename_dup(src_path);
    char* dest_base = bx_path_basename_dup(dest_path);

    if (strcmp(src_base, dest_base) != 0) {
        same_entry = false;
        goto out;
    }

    struct stat src_parent_stat;
    struct stat dest_parent_stat;
    if (!bx_copy_parent_dir_stat(src_path, &src_parent_stat) || !bx_copy_parent_dir_stat(dest_path, &dest_parent_stat)) {
        goto out;
    }

    same_entry = bx_same_file(&src_parent_stat, &dest_parent_stat);

out:
    free(dest_base);
    free(src_base);
    return same_entry;
}

static bool bx_copy_apply_fd_attrs(const struct bx_copy_context* ctx, int src_fd, int dest_fd, const struct stat* src_stat) {
    if (!bx_copy_fd_metadata(src_fd, dest_fd, src_stat, ctx->options->preserve_mask)) {
        bx_perror_path(ctx->diag, "fchown/fchmod/futimens/fsetxattr");
        return false;
    }
    return true;
}

static bool bx_copy_apply_path_attrs(const struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_stat, bool no_follow, bool is_directory) {
    (void)is_directory;
    return bx_copy_path_metadata(src_path, dest_path, src_stat, ctx->options->preserve_mask, no_follow) ? true : (bx_perror_path(ctx->diag, dest_path), false);
}

static unsigned bx_copy_parent_preserve_mask(const struct bx_copy_context* ctx) {
    unsigned mask = ctx->options->preserve_mask & (BX_PRESERVE_MODE | BX_PRESERVE_OWNERSHIP | BX_PRESERVE_TIMESTAMPS);

    if (ctx->options->mode_policy == BX_MODE_POLICY_PRESERVE) {
        mask |= BX_PRESERVE_MODE;
    }

    if (ctx->options->mode_policy == BX_MODE_POLICY_NO_PRESERVE) {
        mask &= ~BX_PRESERVE_MODE;
    }

    return mask;
}

static bool bx_copy_should_skip_existing(const struct bx_copy_options* options,
                                         const char* dest_path,
                                         const struct stat* src_stat,
                                         const struct stat* dest_stat,
                                         bool* skip_out,
                                         struct bx_diag_ctx* diag) {
    enum bx_overwrite_skip_reason reason = BX_OVERWRITE_SKIP_NONE;
    if (!bx_overwrite_should_skip(options->no_clobber, options->interactive, options->update_mode, dest_path, src_stat, dest_stat, skip_out, &reason, diag)) {
        return false;
    }

    if (*skip_out) {
        if (reason == BX_OVERWRITE_SKIP_NO_CLOBBER) {
            bx_debug(diag, "skipping '%s' because of -n", dest_path);
        }
        else if (reason == BX_OVERWRITE_SKIP_UPDATE) {
            bx_debug(diag, "skipping '%s' because of --update", dest_path);
        }
    }
    return true;
}

enum bx_copy_overwrite_result {
    BX_COPY_OVERWRITE_FAILED = 0,
    BX_COPY_OVERWRITE_SKIP,
    BX_COPY_OVERWRITE_CONTINUE,
};

static enum bx_copy_overwrite_result bx_copy_prepare_overwrite(struct bx_copy_context* ctx,
                                                               const char* source_path,
                                                               const char* dest_path,
                                                               const struct stat* src_stat,
                                                               const struct stat* dest_stat,
                                                               struct bx_dest_state* dest_state,
                                                               bool reject_directory_dest,
                                                               bool unlink_existing_dest,
                                                               char** backup_path_out) {
    bool skip = false;

    if (backup_path_out != NULL) {
        *backup_path_out = NULL;
    }

    if (!bx_copy_should_skip_existing(ctx->options, dest_path, src_stat, dest_stat, &skip, ctx->diag)) {
        return BX_COPY_OVERWRITE_FAILED;
    }
    if (skip) {
        return BX_COPY_OVERWRITE_SKIP;
    }

    if (ctx->options->interactive && !bx_prompt_overwrite(ctx->diag->progname, dest_path)) {
        ctx->diag->exit_status = 1;
        return BX_COPY_OVERWRITE_SKIP;
    }

    if (reject_directory_dest && !bx_copy_reject_directory_dest(ctx, source_path, dest_path, dest_state)) {
        return BX_COPY_OVERWRITE_FAILED;
    }

    if (!bx_overwrite_backup_existing(dest_path, &ctx->backup_params, ctx->diag, dest_state, backup_path_out)) {
        return BX_COPY_OVERWRITE_FAILED;
    }

    if (unlink_existing_dest && dest_state->exists_lstat && !bx_copy_unlink_existing_file(ctx, dest_path)) {
        return BX_COPY_OVERWRITE_FAILED;
    }

    return BX_COPY_OVERWRITE_CONTINUE;
}

static bool bx_copy_prepare_link_destination(struct bx_copy_context* ctx, const char* source_path, const char* dest_path, const struct bx_dest_state* dest_state) {
    if (!dest_state->exists_lstat) {
        return true;
    }

    if (S_ISDIR(dest_state->lst.st_mode)) {
        bx_diag(ctx->diag, "cannot overwrite directory '%s' with non-directory '%s'", dest_path, source_path);
        return false;
    }

    if (!(ctx->options->force || ctx->options->remove_destination)) {
        errno = EEXIST;
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    return bx_copy_unlink_existing_file(ctx, dest_path);
}

static struct bx_link_entry* bx_copy_find_link_entry(struct bx_copy_context* ctx, dev_t dev, ino_t ino) {
    struct bx_link_entry* curr = ctx->links;
    while (curr) {
        if (curr->dev == dev && curr->ino == ino) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

static void bx_copy_add_link_entry(struct bx_copy_context* ctx, const struct stat* st, const char* dest_path) {
    if ((ctx->options->preserve_mask & BX_PRESERVE_LINKS) == 0 && !ctx->options->hard_link && !ctx->options->move_mode) {
        return;
    }
    if (st->st_nlink < 2) {
        return;
    }
    if (bx_copy_find_link_entry(ctx, st->st_dev, st->st_ino) != NULL) {
        return;
    }

    struct bx_link_entry* entry = xmalloc(sizeof(*entry));
    entry->dev = st->st_dev;
    entry->ino = st->st_ino;
    entry->dest_path = xstrdup(dest_path);
    entry->next = ctx->links;
    ctx->links = entry;
}

void bx_copy_free_links(struct bx_copy_context* ctx) {
    struct bx_link_entry* curr = ctx->links;
    while (curr) {
        struct bx_link_entry* next = curr->next;
        free(curr->dest_path);
        free(curr);
        curr = next;
    }
    ctx->links = NULL;
}

static void bx_copy_push_source_dir(struct bx_copy_context* ctx, const struct stat* st) {
    struct bx_dir_entry* entry = xmalloc(sizeof(*entry));
    entry->dev = st->st_dev;
    entry->ino = st->st_ino;
    entry->next = ctx->source_dirs;
    ctx->source_dirs = entry;
}

static void bx_copy_pop_source_dir(struct bx_copy_context* ctx) {
    if (ctx->source_dirs) {
        struct bx_dir_entry* top = ctx->source_dirs;
        ctx->source_dirs = top->next;
        free(top);
    }
}

static bool bx_copy_source_dir_in_stack(const struct bx_copy_context* ctx, dev_t dev, ino_t ino) {
    struct bx_dir_entry* curr = ctx->source_dirs;
    while (curr) {
        if (curr->dev == dev && curr->ino == ino) {
            return true;
        }
        curr = curr->next;
    }
    return false;
}

void bx_copy_free_source_dirs(struct bx_copy_context* ctx) {
    while (ctx->source_dirs) {
        bx_copy_pop_source_dir(ctx);
    }
}

void bx_copy_free_parent_attrs(struct bx_copy_context* ctx) {
    struct bx_parent_attr_entry* curr = ctx->parent_attrs;
    while (curr != NULL) {
        struct bx_parent_attr_entry* next = curr->next;
        free(curr->src_path);
        free(curr->dest_path);
        free(curr);
        curr = next;
    }
    ctx->parent_attrs = NULL;
}

static bool bx_copy_record_parent_attr(struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_stat) {
    struct bx_parent_attr_entry* entry = xmalloc(sizeof(*entry));

    entry->src_path = xstrdup(src_path);
    entry->dest_path = xstrdup(dest_path);
    entry->src_stat = *src_stat;
    entry->next = ctx->parent_attrs;
    ctx->parent_attrs = entry;
    return true;
}

static bool bx_copy_apply_parent_attrs(struct bx_copy_context* ctx) {
    unsigned mask = bx_copy_parent_preserve_mask(ctx);

    if (mask == 0u) {
        return true;
    }

    for (struct bx_parent_attr_entry* entry = ctx->parent_attrs; entry != NULL; entry = entry->next) {
        if (!bx_copy_path_metadata(entry->src_path, entry->dest_path, &entry->src_stat, mask, false)) {
            bx_perror_path(ctx->diag, entry->dest_path);
            return false;
        }
    }

    return true;
}

static char* bx_copy_realpath_dup(const char* path) {
    char buf[PATH_MAX];
    if (realpath(path, buf) == NULL) {
        return NULL;
    }
    return xstrdup(buf);
}

static void bx_copy_diag_self_recursive_copy(struct bx_copy_context* ctx, const char* src_path, const char* dest_path) {
    const char* diag_src = ctx->current_source_root ? ctx->current_source_root : src_path;
    const char* diag_dest = ctx->current_dest_root ? ctx->current_dest_root : dest_path;

    bx_diag(ctx->diag, "cannot copy a directory, '%s', into itself, '%s'", diag_src, diag_dest);
    ctx->stop_current_source = true;
}

static void bx_copy_diag_cyclic_symlink(struct bx_copy_context* ctx, const char* src_path) {
    bx_diag(ctx->diag, "cannot copy cyclic symbolic link '%s'", src_path);
}

static char* bx_copy_required_self_copy_child(struct bx_copy_context* ctx, const char* src_path) {
    if (ctx->current_dest_root_realpath == NULL) {
        return NULL;
    }

    char* src_realpath = bx_copy_realpath_dup(src_path);
    if (src_realpath == NULL) {
        return NULL;
    }
    size_t src_len = strlen(src_realpath);
    const char* dest_realpath = ctx->current_dest_root_realpath;

    if (strncmp(dest_realpath, src_realpath, src_len) != 0 || dest_realpath[src_len] != '/') {
        free(src_realpath);
        return NULL;
    }

    const char* suffix = dest_realpath + src_len + 1;
    const char* slash = strchr(suffix, '/');
    size_t component_len = slash ? (size_t)(slash - suffix) : strlen(suffix);

    char* component = xmalloc(component_len + 1);
    memcpy(component, suffix, component_len);
    component[component_len] = '\0';
    free(src_realpath);
    return component;
}

static int bx_copy_data_internal(int src_fd, int dest_fd, struct bx_diag_ctx* diag, const struct bx_copy_options* options) {
    struct bx_copy_data_options data_opts;
    data_opts.sparse_mode = options->sparse_mode;
    data_opts.reflink_mode = options->reflink_mode;

    int res = bx_copy_data(src_fd, dest_fd, &data_opts);
    if (res == BX_COPY_DATA_SUCCESS) {
        return res;
    }
    if (res == BX_COPY_DATA_READ_ERROR) {
        bx_perror_path(diag, "read");
    }
    else if (res == BX_COPY_DATA_WRITE_ERROR) {
        bx_perror_path(diag, "write/lseek/ftruncate");
    }
    else if (res == BX_COPY_DATA_REFLINK_FAILED) {
        bx_diag(diag, "failed to clone '%s'", "destination");
    }
    return res;
}

static bool bx_copy_verify_opened_source(struct bx_copy_context* ctx, const char* src_path, int fd, const struct stat* expected_stat) {
    struct stat opened_stat;

    if (fstat(fd, &opened_stat) != 0) {
        bx_perror_path(ctx->diag, src_path);
        return false;
    }
    if (!bx_same_file(expected_stat, &opened_stat)) {
        bx_diag(ctx->diag, "source '%s' changed during copy", src_path);
        return false;
    }
    return true;
}

static bool bx_copy_verify_selected_source_lstat(struct bx_copy_context* ctx, const char* src_path, const struct stat* expected_stat) {
    struct stat current_stat;

    if (lstat(src_path, &current_stat) != 0) {
        bx_perror_path(ctx->diag, src_path);
        return false;
    }
    if (!bx_same_file(expected_stat, &current_stat)) {
        bx_diag(ctx->diag, "source '%s' changed during copy", src_path);
        return false;
    }
    return true;
}

static bool bx_copy_verify_opened_destination(struct bx_copy_context* ctx, const char* dest_path, int fd, const struct stat* expected_stat) {
    struct stat opened_stat;

    if (fstat(fd, &opened_stat) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    if (!bx_same_file(expected_stat, &opened_stat)) {
        bx_diag(ctx->diag, "destination '%s' changed during copy", dest_path);
        return false;
    }
    return true;
}

static bool bx_copy_verify_destination_directory(struct bx_copy_context* ctx, const char* dest_path, const struct stat* expected_stat) {
    struct stat current_stat;

    if (lstat(dest_path, &current_stat) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    if (!S_ISDIR(current_stat.st_mode) || !bx_same_file(expected_stat, &current_stat)) {
        bx_diag(ctx->diag, "destination directory '%s' changed during copy", dest_path);
        return false;
    }
    return true;
}

static bool bx_copy_verify_destination_socket(struct bx_copy_context* ctx, const char* dest_path, const struct stat* expected_stat) {
    struct stat current_stat;

    if (lstat(dest_path, &current_stat) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    if (!S_ISSOCK(current_stat.st_mode) || !bx_same_file(expected_stat, &current_stat)) {
        bx_diag(ctx->diag, "destination '%s' changed during copy", dest_path);
        return false;
    }
    return true;
}

static bool bx_copy_device_type_matches(mode_t expected_mode, mode_t actual_mode) {
    return (S_ISCHR(expected_mode) && S_ISCHR(actual_mode)) ||
           (S_ISBLK(expected_mode) && S_ISBLK(actual_mode));
}

static bool bx_copy_verify_destination_device(struct bx_copy_context* ctx,
                                              const char* dest_path,
                                              const struct stat* src_stat,
                                              struct stat* created_stat_out) {
    if (fstatat(AT_FDCWD, dest_path, created_stat_out, AT_SYMLINK_NOFOLLOW) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    if (!bx_copy_device_type_matches(src_stat->st_mode, created_stat_out->st_mode) ||
        created_stat_out->st_rdev != src_stat->st_rdev) {
        bx_diag(ctx->diag, "destination '%s' changed during copy", dest_path);
        return false;
    }
    return true;
}

static bool bx_copy_apply_device_fd_path_attrs(struct bx_copy_context* ctx,
                                               const char* src_path,
                                               const char* dest_path,
                                               const struct stat* src_stat,
                                               const struct stat* created_stat) {
#ifdef O_PATH
    int dest_fd = bx_fd_open_nofollow_cloexec(dest_path, O_PATH, 0);
    if (dest_fd < 0) {
        if (errno == ELOOP || errno == ENOENT || errno == ENOTDIR) {
            bx_diag(ctx->diag, "destination '%s' changed during copy", dest_path);
        }
        else {
            bx_perror_path(ctx->diag, dest_path);
        }
        return false;
    }

    bool ok = true;
    struct stat opened_stat;
    if (fstat(dest_fd, &opened_stat) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        ok = false;
        goto out;
    }
    if (!bx_copy_device_type_matches(src_stat->st_mode, opened_stat.st_mode) ||
        opened_stat.st_rdev != src_stat->st_rdev ||
        !bx_same_file(created_stat, &opened_stat)) {
        bx_diag(ctx->diag, "destination '%s' changed during copy", dest_path);
        ok = false;
        goto out;
    }

    char proc_fd_path[64];
    int n = snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%d", dest_fd);
    if (n < 0 || (size_t)n >= sizeof(proc_fd_path)) {
        errno = ENAMETOOLONG;
        bx_perror_path(ctx->diag, dest_path);
        ok = false;
        goto out;
    }

    if (!bx_copy_path_metadata(src_path, proc_fd_path, src_stat, ctx->options->preserve_mask, false)) {
        bx_perror_path(ctx->diag, dest_path);
        ok = false;
        goto out;
    }

    if (fstat(dest_fd, &opened_stat) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        ok = false;
        goto out;
    }
    if (!bx_same_file(created_stat, &opened_stat)) {
        bx_diag(ctx->diag, "destination '%s' changed during copy", dest_path);
        ok = false;
    }

out:
    bx_fd_cleanup(&dest_fd);
    return ok;
#else
    (void)src_path;
    (void)src_stat;
    (void)created_stat;
    errno = ENOTSUP;
    bx_perror_path(ctx->diag, dest_path);
    return false;
#endif
}

static bool bx_copy_cleanup_failed_created_destination(struct bx_copy_context* ctx, const char* dest_path) {
    if (unlink(dest_path) != 0 && errno != ENOENT) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    return true;
}

static bool bx_copy_restore_failed_backup(struct bx_copy_context* ctx, const char* backup_path, const char* dest_path) {
    if (backup_path == NULL) {
        return true;
    }
    if (rename(backup_path, dest_path) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    return true;
}

static bool bx_copy_regular_file(struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_stat, bool open_source_for_attributes_only) {
    enum {
        BX_COPY_REGULAR_PRE_DEST_OPEN = 0,
        BX_COPY_REGULAR_DEST_OPENED,
        BX_COPY_REGULAR_DATA_COPIED,
    } stage = BX_COPY_REGULAR_PRE_DEST_OPEN;
    int src_fd = -1;
    int dest_fd = -1;
    int copy_res = BX_COPY_DATA_SUCCESS;
    struct bx_dest_state dest_state;
    const struct stat* overwrite_stat = NULL;
    char* backup_path = NULL;
    bool created_destination_from_scratch = false;
    mode_t create_mode = bx_copy_regular_file_create_mode(ctx, src_stat);

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_stat && bx_same_file(src_stat, &dest_state.st)) {
        if (ctx->backup_params.mode != BX_BACKUP_NONE && ctx->options->force && S_ISREG(src_stat->st_mode)) {
            if (strcmp(src_path, dest_path) == 0) {
                enum bx_backup_create_result backup_result = bx_copy_backup_same_file_copy(ctx, src_path, dest_path);
                if (backup_result == BX_BACKUP_CREATE_CREATED) {
                    return true;
                }
                if (backup_result == BX_BACKUP_CREATE_FAILED) {
                    return false;
                }
            }
            else if (!bx_copy_paths_name_same_directory_entry(src_path, dest_path)) {
                /*
                 * GNU cp lets --force --backup replace an alternate destination
                 * entry that resolves to the same regular file (for example via
                 * a hard link or a symlink). The exact same directory entry
                 * still errors unless the operand strings are identical, which
                 * is handled by the special-case backup copy above.
                 */
            }
            else {
                bx_debug(ctx->diag, "skipping '%s' because it is the same file as '%s'", src_path, dest_path);
                bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
                return false;
            }
        }
        else {
            bx_debug(ctx->diag, "skipping '%s' because it is the same file as '%s'", src_path, dest_path);
            bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
            return false;
        }
    }

    overwrite_stat = bx_copy_overwrite_dest_stat(ctx, &dest_state);

    if (dest_state.dangling_symlink && !ctx->options->remove_destination && overwrite_stat == NULL) {
        bx_diag(ctx->diag, "not writing through dangling symlink '%s'", dest_path);
        return false;
    }

    if (overwrite_stat != NULL) {
        enum bx_copy_overwrite_result result = bx_copy_prepare_overwrite(ctx, src_path, dest_path, src_stat, overwrite_stat, &dest_state, true, false, &backup_path);
        if (result == BX_COPY_OVERWRITE_FAILED) {
            return false;
        }
        if (result == BX_COPY_OVERWRITE_SKIP) {
            return true;
        }
    }

    if (!ctx->options->attributes_only || open_source_for_attributes_only || (ctx->options->preserve_mask & (BX_PRESERVE_XATTR | BX_PRESERVE_MODE)) != 0u) {
        src_fd = bx_fd_open_read(src_path, ctx->diag);
        if (src_fd < 0) {
            goto fail;
        }
        if (S_ISREG(src_stat->st_mode) && !bx_copy_verify_opened_source(ctx, src_path, src_fd, src_stat)) {
            goto fail;
        }
    }

    if (ctx->options->remove_destination && dest_state.exists_lstat) {
        if (!bx_copy_unlink_existing_file(ctx, dest_path)) {
            goto fail;
        }
        memset(&dest_state, 0, sizeof(dest_state));
    }

    created_destination_from_scratch = !dest_state.exists_lstat;

    int dest_open_flags = O_CREAT;
    if (!ctx->options->attributes_only) {
        dest_open_flags |= O_TRUNC;
    }

    dest_fd = bx_fd_open_write(dest_path, dest_open_flags, create_mode, (ctx->options->force && dest_state.exists_lstat) ? NULL : ctx->diag);
    if (dest_fd < 0 && ctx->options->force && dest_state.exists_lstat) {
        if (bx_copy_unlink_existing_file(ctx, dest_path)) {
            dest_fd = bx_fd_open_write(dest_path, dest_open_flags, create_mode, ctx->diag);
        }
    }
    if (dest_fd < 0) {
        if (dest_state.dangling_symlink && !ctx->options->force && !ctx->options->remove_destination) {
            bx_diag(ctx->diag, "not writing through dangling symlink '%s'", dest_path);
        }
        goto fail;
    }
    stage = BX_COPY_REGULAR_DEST_OPENED;

    if (!ctx->options->attributes_only) {
        copy_res = bx_copy_data_internal(src_fd, dest_fd, ctx->diag, ctx->options);
        if (copy_res != BX_COPY_DATA_SUCCESS) {
            goto fail;
        }
    }
    stage = BX_COPY_REGULAR_DATA_COPIED;
    if (!bx_copy_apply_fd_attrs(ctx, src_fd, dest_fd, src_stat)) {
        goto fail;
    }

    if (!bx_fd_close(&dest_fd, dest_path, ctx->diag)) {
        goto fail;
    }

    if (src_fd >= 0 && !bx_fd_close(&src_fd, src_path, ctx->diag)) {
        return false;
    }

    bx_copy_add_link_entry(ctx, src_stat, dest_path);
    bx_info(ctx->diag, "'%s' -> '%s'", src_path, dest_path);
    free(backup_path);
    return true;

fail:
    bx_fd_cleanup(&src_fd);
    bx_fd_cleanup(&dest_fd);
    if (copy_res == BX_COPY_DATA_REFLINK_FAILED && created_destination_from_scratch) {
        if (!bx_copy_cleanup_failed_created_destination(ctx, dest_path)) {
            free(backup_path);
            return false;
        }
    }
    if ((backup_path != NULL && stage == BX_COPY_REGULAR_PRE_DEST_OPEN) || (backup_path != NULL && copy_res == BX_COPY_DATA_REFLINK_FAILED)) {
        if (!bx_copy_restore_failed_backup(ctx, backup_path, dest_path)) {
            free(backup_path);
            return false;
        }
    }
    free(backup_path);
    return false;
}

static bool bx_copy_regular_file_path(struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_stat) {
    return bx_copy_regular_file(ctx, src_path, dest_path, src_stat, false);
}

static bool bx_copy_fifo_contents(struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_stat) {
    return bx_copy_regular_file(ctx, src_path, dest_path, src_stat, true);
}

static bool bx_copy_create_fifo_node(struct bx_copy_context* ctx, const char* dest_path, mode_t create_mode) {
    if (mkfifo(dest_path, create_mode) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    return true;
}

static int bx_copy_open_fifo_metadata_fd(struct bx_copy_context* ctx, const char* path) {
    int fd = bx_fd_open_nofollow_cloexec(path, O_RDONLY | O_NONBLOCK, 0);

    if (fd < 0) {
        bx_perror_path(ctx->diag, path);
    }
    return fd;
}

static bool bx_copy_apply_fifo_fd_attrs(struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_stat, const struct stat* dest_stat) {
    int src_fd = -1;
    int dest_fd = -1;
    bool ok = true;

    if ((ctx->options->preserve_mask & (BX_PRESERVE_MODE | BX_PRESERVE_XATTR)) != 0u) {
        src_fd = bx_copy_open_fifo_metadata_fd(ctx, src_path);
        if (src_fd < 0) {
            ok = false;
            goto out;
        }
        if (!bx_copy_verify_opened_source(ctx, src_path, src_fd, src_stat)) {
            ok = false;
            goto out;
        }
    }

    dest_fd = bx_copy_open_fifo_metadata_fd(ctx, dest_path);
    if (dest_fd < 0) {
        ok = false;
        goto out;
    }
    if (!bx_copy_verify_opened_destination(ctx, dest_path, dest_fd, dest_stat)) {
        ok = false;
        goto out;
    }

    if (!bx_copy_apply_fd_attrs(ctx, src_fd, dest_fd, src_stat)) {
        ok = false;
    }

out:
    bx_fd_cleanup(&src_fd);
    bx_fd_cleanup(&dest_fd);
    return ok;
}

static bool bx_copy_create_socket_node(struct bx_copy_context* ctx, const char* dest_path, mode_t create_mode) {
    size_t path_len = strlen(dest_path);
    struct sockaddr_un addr;
    int fd;

    if (path_len >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    fd = bx_fd_socket_cloexec(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, dest_path, path_len + 1u);

    mode_t socket_umask = (mode_t)(0777u & ~(create_mode & 0777u));
    mode_t old_umask = umask(socket_umask);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1u);
    int bind_rc = bind(fd, (const struct sockaddr*)&addr, addr_len);
    int bind_errno = errno;
    umask(old_umask);
    errno = bind_errno;

    if (bind_rc != 0) {
        bx_perror_path(ctx->diag, dest_path);
        bx_fd_cleanup(&fd);
        return false;
    }

    if (!bx_fd_close(&fd, dest_path, ctx->diag)) {
        return false;
    }

    return true;
}

static bool bx_copy_create_device_node(struct bx_copy_context* ctx, const char* dest_path, mode_t create_mode, dev_t rdev) {
    if (bx_fd_mknodat(AT_FDCWD, dest_path, create_mode, rdev) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    return true;
}

static bool bx_copy_device_node(struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_stat) {
    struct bx_dest_state dest_state;
    mode_t create_mode = bx_copy_regular_file_create_mode(ctx, src_stat);

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_lstat && bx_same_file(src_stat, &dest_state.lst)) {
        bx_debug(ctx->diag, "skipping '%s' because it is the same file as '%s'", src_path, dest_path);
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        enum bx_copy_overwrite_result result = bx_copy_prepare_overwrite(ctx, src_path, dest_path, src_stat, &dest_state.lst, &dest_state, true, true, NULL);
        if (result == BX_COPY_OVERWRITE_FAILED) {
            return false;
        }
        if (result == BX_COPY_OVERWRITE_SKIP) {
            return true;
        }
    }

    if (!bx_copy_create_device_node(ctx, dest_path, create_mode, src_stat->st_rdev)) {
        return false;
    }

    struct stat created_stat;
    if (!bx_copy_verify_destination_device(ctx, dest_path, src_stat, &created_stat)) {
        return false;
    }

    if (!bx_copy_apply_device_fd_path_attrs(ctx, src_path, dest_path, src_stat, &created_stat)) {
        return false;
    }

    bx_copy_add_link_entry(ctx, src_stat, dest_path);
    bx_info(ctx->diag, "'%s' -> '%s'", src_path, dest_path);
    return true;
}

static bool bx_copy_fifo(struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_stat) {
    struct bx_dest_state dest_state;
    struct stat created_stat;
    mode_t create_mode = bx_copy_regular_file_create_mode(ctx, src_stat);

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_lstat && bx_same_file(src_stat, &dest_state.lst)) {
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        enum bx_copy_overwrite_result result = bx_copy_prepare_overwrite(ctx, src_path, dest_path, src_stat, &dest_state.lst, &dest_state, true, true, NULL);
        if (result == BX_COPY_OVERWRITE_FAILED) {
            return false;
        }
        if (result == BX_COPY_OVERWRITE_SKIP) {
            return true;
        }
    }

    if (!bx_copy_create_fifo_node(ctx, dest_path, create_mode)) {
        return false;
    }

    if (lstat(dest_path, &created_stat) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    if (!S_ISFIFO(created_stat.st_mode)) {
        bx_diag(ctx->diag, "destination '%s' changed during copy", dest_path);
        return false;
    }

    if (!bx_copy_apply_fifo_fd_attrs(ctx, src_path, dest_path, src_stat, &created_stat)) {
        return false;
    }

    bx_copy_add_link_entry(ctx, src_stat, dest_path);
    bx_info(ctx->diag, "'%s' -> '%s'", src_path, dest_path);
    return true;
}

static bool bx_copy_socket_contents(struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_stat) {
    return bx_copy_regular_file(ctx, src_path, dest_path, src_stat, true);
}

static bool bx_copy_socket(struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_stat) {
    struct bx_dest_state dest_state;
    struct stat created_stat;
    mode_t create_mode = bx_copy_regular_file_create_mode(ctx, src_stat);

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_lstat && bx_same_file(src_stat, &dest_state.lst)) {
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        enum bx_copy_overwrite_result result = bx_copy_prepare_overwrite(ctx, src_path, dest_path, src_stat, &dest_state.lst, &dest_state, true, true, NULL);
        if (result == BX_COPY_OVERWRITE_FAILED) {
            return false;
        }
        if (result == BX_COPY_OVERWRITE_SKIP) {
            return true;
        }
    }

    if (!bx_copy_create_socket_node(ctx, dest_path, create_mode)) {
        return false;
    }

    if (lstat(dest_path, &created_stat) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    if (!S_ISSOCK(created_stat.st_mode)) {
        bx_diag(ctx->diag, "destination '%s' changed during copy", dest_path);
        return false;
    }

    if (!bx_copy_apply_path_attrs(ctx, src_path, dest_path, src_stat, true, false)) {
        return false;
    }
    if (!bx_copy_verify_destination_socket(ctx, dest_path, &created_stat)) {
        return false;
    }

    bx_copy_add_link_entry(ctx, src_stat, dest_path);
    bx_info(ctx->diag, "'%s' -> '%s'", src_path, dest_path);
    return true;
}

static bool bx_copy_symlink_object(struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_lstat) {
    struct bx_dest_state dest_state;
    ssize_t target_size = src_lstat->st_size > 0 ? src_lstat->st_size : 256;
    char* link_target = NULL;

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_lstat && bx_same_file(src_lstat, &dest_state.lst)) {
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        enum bx_copy_overwrite_result result = bx_copy_prepare_overwrite(ctx, src_path, dest_path, src_lstat, &dest_state.lst, &dest_state, true, true, NULL);
        if (result == BX_COPY_OVERWRITE_FAILED) {
            return false;
        }
        if (result == BX_COPY_OVERWRITE_SKIP) {
            return true;
        }
    }

    while (true) {
        link_target = xrealloc(link_target, (size_t)target_size + 1u);
        ssize_t nread = readlink(src_path, link_target, (size_t)target_size);
        if (nread < 0) {
            free(link_target);
            bx_perror_path(ctx->diag, src_path);
            return false;
        }
        if (nread < target_size) {
            link_target[nread] = '\0';
            break;
        }
        target_size *= 2;
    }

    if (symlink(link_target, dest_path) != 0) {
        free(link_target);
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (!bx_copy_apply_path_attrs(ctx, src_path, dest_path, src_lstat, true, false)) {
        free(link_target);
        return false;
    }

    bx_copy_add_link_entry(ctx, src_lstat, dest_path);
    bx_info(ctx->diag, "'%s' -> '%s'", src_path, dest_path);
    free(link_target);
    return true;
}

static bool bx_copy_create_symbolic_link(struct bx_copy_context* ctx, const char* source_operand, const char* dest_path, const struct stat* src_stat) {
    struct bx_dest_state dest_state;
    const struct stat* overwrite_stat = NULL;

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_stat && bx_same_file(src_stat, &dest_state.st)) {
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", source_operand, dest_path);
        return false;
    }

    overwrite_stat = bx_copy_overwrite_dest_stat(ctx, &dest_state);

    if (dest_state.exists_lstat) {
        if (overwrite_stat != NULL) {
            enum bx_copy_overwrite_result result = bx_copy_prepare_overwrite(ctx, source_operand, dest_path, src_stat, overwrite_stat, &dest_state, false, false, NULL);
            if (result == BX_COPY_OVERWRITE_FAILED) {
                return false;
            }
            if (result == BX_COPY_OVERWRITE_SKIP) {
                return true;
            }
        }
        if (!bx_copy_prepare_link_destination(ctx, source_operand, dest_path, &dest_state)) {
            return false;
        }
    }

    if (source_operand[0] != '/' && !bx_copy_relative_symlink_stays_in_cwd(dest_path)) {
        bx_diag(ctx->diag, "%s: can make relative symbolic links only in current directory", dest_path);
        return false;
    }

    if (symlink(source_operand, dest_path) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    bx_info(ctx->diag, "'%s' -> '%s'", source_operand, dest_path);
    return true;
}

static bool bx_copy_create_hard_link(struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_stat, bool follow_source) {
    struct bx_dest_state dest_state;
    const struct stat* overwrite_stat = NULL;

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_stat && bx_same_file(src_stat, &dest_state.st)) {
        bx_debug(ctx->diag, "skipping '%s' because it is the same file as '%s'", src_path, dest_path);
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    overwrite_stat = bx_copy_overwrite_dest_stat(ctx, &dest_state);

    if (overwrite_stat != NULL) {
        enum bx_copy_overwrite_result result = bx_copy_prepare_overwrite(ctx, src_path, dest_path, src_stat, overwrite_stat, &dest_state, false, false, NULL);
        if (result == BX_COPY_OVERWRITE_FAILED) {
            return false;
        }
        if (result == BX_COPY_OVERWRITE_SKIP) {
            return true;
        }
    }

    if (!bx_copy_prepare_link_destination(ctx, src_path, dest_path, &dest_state)) {
        return false;
    }

    if (linkat(AT_FDCWD, src_path, AT_FDCWD, dest_path, follow_source ? AT_SYMLINK_FOLLOW : 0) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    bx_info(ctx->diag, "'%s' -> '%s'", src_path, dest_path);
    return true;
}

static bool bx_copy_prepare_parents(struct bx_copy_context* ctx, const char* source_operand) {
    char* src_copy = xstrdup(source_operand);
    size_t len = strlen(src_copy);
    size_t start = 0;
    unsigned parent_mask = bx_copy_parent_preserve_mask(ctx);

    if (len == 0) {
        free(src_copy);
        return true;
    }
    if (src_copy[0] == '/') {
        start = 1;
    }

    for (size_t i = start; src_copy[i] != '\0'; i++) {
        if (src_copy[i] != '/') {
            continue;
        }
        src_copy[i] = '\0';
        if (src_copy[0] != '\0' || start == 1) {
            char* current_src = (start == 1 && src_copy[0] == '\0') ? "/" : src_copy;
            char* current_dest = bx_path_join(ctx->target_root, current_src);
            struct stat src_st;
            mode_t mkdir_mode;
            mode_t final_mode = 0;
            bool restore_mode = false;

            if (stat(current_src, &src_st) != 0) {
                bx_perror_path(ctx->diag, current_src);
                free(current_dest);
                free(src_copy);
                return false;
            }

            mkdir_mode = bx_copy_directory_create_mode(ctx, &src_st, &final_mode, &restore_mode);
            (void)final_mode;
            (void)restore_mode;
            if (mkdir(current_dest, mkdir_mode) != 0) {
                if (errno != EEXIST) {
                    bx_perror_path(ctx->diag, current_dest);
                    free(current_dest);
                    free(src_copy);
                    return false;
                }
                struct stat dest_st;
                if (stat(current_dest, &dest_st) != 0) {
                    bx_perror_path(ctx->diag, current_dest);
                    free(current_dest);
                    free(src_copy);
                    return false;
                }
                if (!S_ISDIR(dest_st.st_mode)) {
                    bx_diag(ctx->diag, "cannot create directory '%s': Not a directory", current_dest);
                    free(current_dest);
                    free(src_copy);
                    return false;
                }
            }

            if (parent_mask != 0u && !bx_copy_record_parent_attr(ctx, current_src, current_dest, &src_st)) {
                free(current_dest);
                free(src_copy);
                return false;
            }
            free(current_dest);
        }
        src_copy[i] = '/';
    }

    free(src_copy);
    return true;
}

static bool bx_copy_path_selected(struct bx_copy_context* ctx,
                                  const char* src_path,
                                  const char* source_operand,
                                  const char* dest_path,
                                  bool top_level,
                                  const struct stat* selected_lstat);

static bool bx_copy_directory(struct bx_copy_context* ctx, const char* src_path, const char* dest_path, const struct stat* src_stat, bool top_level) {
    struct bx_dest_state dest_state;
    bool created = false;
    bool restore_mode = false;
    mode_t final_mode = 0;
    bool prev_dest_root_active = ctx->dest_root_active;
    dev_t prev_dest_root_dev = ctx->dest_root_dev;
    ino_t prev_dest_root_ino = ctx->dest_root_ino;
    DIR* dir = NULL;
    bool ok = true;
    bool dest_dir_changed = false;

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        if (!S_ISDIR(dest_state.lst.st_mode)) {
            bx_diag(ctx->diag, "cannot overwrite non-directory '%s' with directory '%s'", dest_path, src_path);
            return false;
        }
    }
    else {
        mode_t mkdir_mode = bx_copy_directory_create_mode(ctx, src_stat, &final_mode, &restore_mode);
        if (mkdir(dest_path, mkdir_mode) != 0) {
            bx_perror_path(ctx->diag, dest_path);
            return false;
        }
        created = true;
        if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
            bx_perror_path(ctx->diag, dest_path);
            return false;
        }
    }

    if (top_level) {
        ctx->dest_root_active = true;
        ctx->dest_root_dev = dest_state.lst.st_dev;
        ctx->dest_root_ino = dest_state.lst.st_ino;
        free(ctx->current_dest_root_realpath);
        ctx->current_dest_root_realpath = bx_copy_realpath_dup(dest_path);
    }

    bx_copy_push_source_dir(ctx, src_stat);

    dir = opendir(src_path);
    if (dir == NULL) {
        bx_perror_path(ctx->diag, src_path);
        ok = false;
        goto finish;
    }
    if (!bx_copy_verify_opened_source(ctx, src_path, dirfd(dir), src_stat)) {
        ok = false;
        goto finish;
    }

    char* required_child = bx_copy_required_self_copy_child(ctx, src_path);
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                bx_perror_path(ctx->diag, src_path);
                ok = false;
            }
            break;
        }
        if (bx_path_is_dot_or_dotdot(entry->d_name)) {
            continue;
        }
        if (required_child != NULL && strcmp(entry->d_name, required_child) != 0) {
            continue;
        }

        if (!bx_copy_verify_destination_directory(ctx, dest_path, &dest_state.lst)) {
            dest_dir_changed = true;
            ok = false;
            break;
        }

        char* src_child = bx_path_join(src_path, entry->d_name);
        struct stat child_lstat;
        if (lstat(src_child, &child_lstat) != 0) {
            bx_perror_path(ctx->diag, src_child);
            free(src_child);
            ok = false;
            continue;
        }
        if (ctx->dest_root_active && S_ISDIR(child_lstat.st_mode) && child_lstat.st_dev == ctx->dest_root_dev && child_lstat.st_ino == ctx->dest_root_ino) {
            bx_copy_diag_self_recursive_copy(ctx, src_path, dest_path);
            free(src_child);
            ok = false;
            continue;
        }

        char* dest_child = bx_path_join(dest_path, entry->d_name);
        if (!bx_copy_path_selected(ctx, src_child, src_child, dest_child, false, &child_lstat)) {
            ok = false;
            free(dest_child);
            free(src_child);
            if (ctx->stop_current_source) {
                break;
            }
            continue;
        }
        free(dest_child);
        free(src_child);
    }
    free(required_child);

finish:
    if (dir != NULL) {
        if (closedir(dir) != 0) {
            bx_perror_path(ctx->diag, src_path);
            ok = false;
        }
        dir = NULL;
    }

    if (!dest_dir_changed && !bx_copy_verify_destination_directory(ctx, dest_path, &dest_state.lst)) {
        dest_dir_changed = true;
        ok = false;
    }

    if (!dest_dir_changed && created && restore_mode) {
        if (chmod(dest_path, final_mode) != 0) {
            bx_perror_path(ctx->diag, dest_path);
            ok = false;
        }
    }
    if (!dest_dir_changed && !bx_copy_apply_path_attrs(ctx, src_path, dest_path, src_stat, false, true)) {
        ok = false;
    }
    if (ok && created) {
        bx_info(ctx->diag, "'%s' -> '%s'", src_path, dest_path);
    }

    bx_copy_pop_source_dir(ctx);
    ctx->dest_root_active = prev_dest_root_active;
    ctx->dest_root_dev = prev_dest_root_dev;
    ctx->dest_root_ino = prev_dest_root_ino;
    return ok;
}

static bool bx_copy_path_selected(struct bx_copy_context* ctx,
                                  const char* src_path,
                                  const char* source_operand,
                                  const char* dest_path,
                                  bool top_level,
                                  const struct stat* selected_lstat) {
    struct stat src_lstat;
    struct stat src_stat;
    bool source_is_symlink;
    bool follow_source;
    bool ok = false;
    bool parents_prepared = false;

    if (selected_lstat != NULL) {
        src_lstat = *selected_lstat;
        if (!bx_copy_verify_selected_source_lstat(ctx, src_path, selected_lstat)) {
            return false;
        }
    }
    else {
        if (lstat(src_path, &src_lstat) != 0) {
            bx_perror_path(ctx->diag, src_path);
            return false;
        }
    }

    if (!top_level && ctx->options->one_file_system && src_lstat.st_dev != ctx->source_root_dev) {
        return true;
    }

    source_is_symlink = S_ISLNK(src_lstat.st_mode);
    follow_source = bx_copy_should_follow_source(ctx->options, top_level, source_is_symlink);

    if (follow_source) {
        if (stat(src_path, &src_stat) != 0) {
            bx_perror_path(ctx->diag, src_path);
            return false;
        }
    }
    else {
        src_stat = src_lstat;
    }

    if (top_level) {
        ctx->source_root_dev = src_stat.st_dev;
    }

    if (follow_source && source_is_symlink && S_ISDIR(src_stat.st_mode) && bx_copy_source_dir_in_stack(ctx, src_stat.st_dev, src_stat.st_ino)) {
        bx_copy_diag_cyclic_symlink(ctx, src_path);
        return false;
    }

    if (top_level && ctx->options->parents) {
        bx_copy_free_parent_attrs(ctx);
        if (!bx_copy_prepare_parents(ctx, source_operand)) {
            bx_copy_free_parent_attrs(ctx);
            return false;
        }
        parents_prepared = true;
    }

    if (S_ISDIR(src_stat.st_mode)) {
        if (!ctx->options->recursive) {
            bx_diag(ctx->diag, "-r not specified; omitting directory '%s'", src_path);
            goto finish;
        }
        ok = bx_copy_directory(ctx, src_path, dest_path, &src_stat, top_level);
        goto finish;
    }

    if (ctx->options->symbolic_link) {
        ok = bx_copy_create_symbolic_link(ctx, source_operand, dest_path, &src_stat);
        goto finish;
    }
    if (ctx->options->hard_link) {
        ok = bx_copy_create_hard_link(ctx, src_path, dest_path, &src_stat, follow_source);
        goto finish;
    }

    if ((ctx->options->preserve_mask & BX_PRESERVE_LINKS) != 0u && !ctx->options->hard_link && !ctx->options->symbolic_link) {
        struct bx_link_entry* entry = bx_copy_find_link_entry(ctx, src_stat.st_dev, src_stat.st_ino);
        if (entry != NULL) {
            ok = bx_copy_create_hard_link(ctx, entry->dest_path, dest_path, &src_stat, false);
            goto finish;
        }
    }

    if (!follow_source && S_ISLNK(src_lstat.st_mode)) {
        ok = bx_copy_symlink_object(ctx, src_path, dest_path, &src_lstat);
        goto finish;
    }
    if (S_ISFIFO(src_stat.st_mode)) {
        if (!ctx->options->recursive || ctx->options->copy_contents) {
            ok = bx_copy_fifo_contents(ctx, src_path, dest_path, &src_stat);
            goto finish;
        }
        ok = bx_copy_fifo(ctx, src_path, dest_path, &src_stat);
        goto finish;
    }
    if (S_ISSOCK(src_stat.st_mode)) {
        if (!ctx->options->recursive || ctx->options->copy_contents) {
            ok = bx_copy_socket_contents(ctx, src_path, dest_path, &src_stat);
            goto finish;
        }
        ok = bx_copy_socket(ctx, src_path, dest_path, &src_stat);
        goto finish;
    }
    if (S_ISCHR(src_stat.st_mode) || S_ISBLK(src_stat.st_mode)) {
        if (ctx->options->copy_contents) {
            ok = bx_copy_regular_file_path(ctx, src_path, dest_path, &src_stat);
            goto finish;
        }
        ok = bx_copy_device_node(ctx, src_path, dest_path, &src_stat);
        goto finish;
    }
    if (S_ISREG(src_stat.st_mode)) {
        ok = bx_copy_regular_file_path(ctx, src_path, dest_path, &src_stat);
        goto finish;
    }

    bx_diag(ctx->diag, "unsupported file type for '%s'", src_path);
finish:
    if (parents_prepared) {
        if (ok && !bx_copy_apply_parent_attrs(ctx)) {
            ok = false;
        }
        bx_copy_free_parent_attrs(ctx);
    }

    return ok;
}

bool bx_copy_path(struct bx_copy_context* ctx, const char* src_path, const char* source_operand, const char* dest_path, bool top_level) {
    return bx_copy_path_selected(ctx, src_path, source_operand, dest_path, top_level, NULL);
}
