#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets/archive/archive_fs.h"
#include "bx/libbx.h"
#include "lib/dir_path.h"
#include "lib/fd_ops.h"
#include "lib/mount_identity.h"
#include "lib/path_ops.h"

struct bx_archive_fs_path_buf {
    char* data;
    size_t len;
    size_t cap;
};

struct bx_archive_fs_boundary {
    bool enabled;
    bool initialized;
    struct bx_mount_identity root;
};

static void bx_archive_fs_entry_free(struct bx_archive_fs_entry* entry) {
    free(entry->source_path);
    free(entry->archive_path);
    free(entry->link_target);
    entry->source_path = NULL;
    entry->archive_path = NULL;
    entry->link_target = NULL;
}

void bx_archive_fs_list_free(struct bx_archive_fs_list* list) {
    size_t i;
    for (i = 0u; i < list->len; i++) {
        bx_archive_fs_entry_free(&list->entries[i]);
    }
    free(list->entries);
    list->entries = NULL;
    list->len = 0u;
    list->cap = 0u;
}

static bool bx_archive_fs_list_push(struct bx_archive_fs_list* list,
                                    const char* source_path,
                                    const char* archive_path,
                                    const struct stat* st,
                                    const char* link_target) {
    struct bx_archive_fs_entry* entry;

    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 32u;
        list->entries = xrealloc(list->entries, next_cap * sizeof(*list->entries));
        list->cap = next_cap;
    }

    entry = &list->entries[list->len++];
    *entry = (struct bx_archive_fs_entry){0};
    entry->source_path = xstrdup(source_path);
    entry->archive_path = xstrdup(archive_path);
    entry->st = *st;
    entry->link_target = link_target ? xstrdup(link_target) : NULL;
    return true;
}

static bool bx_archive_fs_collect_entry(const struct bx_archive_fs_visit_entry* entry,
                                        void* user_data,
                                        struct bx_diag_ctx* diag) {
    struct bx_archive_fs_list* list = user_data;
    (void)diag;

    return bx_archive_fs_list_push(list,
                                   entry->source_path,
                                   entry->archive_path,
                                   entry->st,
                                   entry->link_target);
}

static int bx_archive_name_compare(const void* left, const void* right) {
    const char* const* a = left;
    const char* const* b = right;
    return strcmp(*a, *b);
}

static void bx_archive_fs_path_buf_cleanup(struct bx_archive_fs_path_buf* buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0u;
    buf->cap = 0u;
}

static void bx_archive_fs_path_buf_init(struct bx_archive_fs_path_buf* buf, const char* path) {
    buf->len = strlen(path);
    buf->cap = buf->len + 1u;
    buf->data = xmalloc(buf->cap);
    memcpy(buf->data, path, buf->cap);
}

static void bx_archive_fs_path_buf_reserve(struct bx_archive_fs_path_buf* buf, size_t need_len) {
    size_t need_cap = need_len + 1u;

    if (need_cap <= buf->cap) {
        return;
    }
    while (buf->cap < need_cap) {
        buf->cap *= 2u;
    }
    buf->data = xrealloc(buf->data, buf->cap);
}

static size_t bx_archive_fs_path_buf_push_child(struct bx_archive_fs_path_buf* buf, const char* name) {
    size_t restore_len = buf->len;
    size_t name_len = strlen(name);
    bool need_slash = buf->len > 0u && buf->data[buf->len - 1u] != '/';
    size_t new_len = buf->len + (need_slash ? 1u : 0u) + name_len;

    bx_archive_fs_path_buf_reserve(buf, new_len);
    if (need_slash) {
        buf->data[buf->len++] = '/';
    }
    memcpy(buf->data + buf->len, name, name_len);
    buf->len = new_len;
    buf->data[buf->len] = '\0';
    return restore_len;
}

static void bx_archive_fs_path_buf_restore(struct bx_archive_fs_path_buf* buf, size_t restore_len) {
    buf->len = restore_len;
    buf->data[buf->len] = '\0';
}

static enum bx_archive_fs_error_action
bx_archive_fs_handle_error(const char* path,
                           enum bx_archive_fs_error_op op,
                           int errnum,
                           bx_archive_fs_error_fn error_fn,
                           void* error_user_data,
                           struct bx_diag_ctx* diag) {
    if (error_fn != NULL) {
        return error_fn(path, op, errnum, error_user_data);
    }

    bx_diag(diag, "%s: %s", path, strerror(errnum));
    return BX_ARCHIVE_FS_ERROR_ABORT;
}

struct bx_archive_fs_visit_state {
    struct bx_archive_fs_path_buf source;
    struct bx_archive_fs_path_buf archive;
    struct bx_archive_fs_boundary boundary;
    bool sort_children;
    bx_archive_fs_include_fn include_fn;
    void* include_user_data;
    bx_archive_fs_error_fn error_fn;
    void* error_user_data;
    bx_archive_fs_visit_fn visit_fn;
    void* visit_user_data;
    struct bx_diag_ctx* diag;
};

static bool bx_archive_fs_visit_error(struct bx_archive_fs_visit_state* state,
                                       enum bx_archive_fs_error_op op, int error) {
    return bx_archive_fs_handle_error(state->source.data, op, error,
                                       state->error_fn, state->error_user_data,
                                       state->diag) == BX_ARCHIVE_FS_ERROR_SKIP;
}

/* A NULL result means the caller must not descend (boundary or skipped error).
 * Keep the checked directory open through child lookup, including sorted walks. */
static bool bx_archive_fs_open_dir(struct bx_archive_fs_visit_state* state,
                                   int parent_fd, const char* name,
                                   const struct stat* expected, DIR** result) {
    *result = NULL;
    int fd = bx_fd_openat_cloexec(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    if (fd < 0)
        return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_OPENDIR, errno);
    struct stat opened;
    if (bx_fd_fstat_expected(fd, expected, &opened) != 0) {
        int error = errno;
        close(fd);
        return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_LSTAT, error);
    }
    if (state->boundary.enabled) {
        struct bx_mount_identity identity;
        bool same = true;
        bool ok = bx_mount_identity_read(fd, &identity);
        if (ok && state->boundary.initialized)
            ok = bx_mount_identity_compare(&state->boundary.root, &identity, &same);
        if (!ok || !same) {
            int error = errno;
            int rc = close(fd);
            if (!ok)
                return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_LSTAT, error);
            if (rc != 0)
                return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_CLOSEDIR, errno);
            return true;
        }
        if (!state->boundary.initialized) {
            state->boundary.root = identity;
            state->boundary.initialized = true;
        }
    }
    *result = fdopendir(fd);
    if (!*result) {
        int error = errno;
        close(fd);
        return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_OPENDIR, error);
    }
    return true;
}

static bool bx_archive_fs_visit_inner(struct bx_archive_fs_visit_state* state,
                                       int parent_fd, const char* name, bool recurse);

static bool bx_archive_fs_visit_children(struct bx_archive_fs_visit_state* state, DIR* dir) {
    char** names = NULL;
    size_t len = 0, cap = 0, index = 0;
    bool ok = true;
    struct dirent* entry;
    if (state->sort_children) {
        for (;;) {
            errno = 0;
            entry = readdir(dir);
            if (!entry) {
                if (errno)
                    ok = bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_READDIR, errno);
                break;
            }
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            if (len == cap) {
                cap = cap ? cap * 2u : 16u;
                names = xrealloc(names, cap * sizeof(*names));
            }
            names[len++] = xstrdup(entry->d_name);
        }
        if (len > 1)
            qsort(names, len, sizeof(*names), bx_archive_name_compare);
    }
    while (ok) {
        const char* name;
        if (state->sort_children) {
            if (index == len)
                break;
            name = names[index++];
        }
        else {
            errno = 0;
            entry = readdir(dir);
            if (!entry) {
                if (errno)
                    ok = bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_READDIR, errno);
                break;
            }
            name = entry->d_name;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
        }
        size_t source_len = bx_archive_fs_path_buf_push_child(&state->source, name);
        size_t archive_len = bx_archive_fs_path_buf_push_child(&state->archive, name);
        ok = bx_archive_fs_visit_inner(state, dirfd(dir), name, true);
        bx_archive_fs_path_buf_restore(&state->archive, archive_len);
        bx_archive_fs_path_buf_restore(&state->source, source_len);
    }
    for (size_t i = 0; i < len; i++)
        free(names[i]);
    free(names);
    if (closedir(dir) != 0 && ok)
        ok = bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_CLOSEDIR, errno);
    return ok;
}

static bool bx_archive_fs_visit_inner(struct bx_archive_fs_visit_state* state,
                                       int parent_fd, const char* name, bool recurse) {
    struct stat status;
    if (fstatat(parent_fd, name, &status, AT_SYMLINK_NOFOLLOW) != 0)
        return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_LSTAT, errno);
    struct bx_archive_fs_visit_entry visit = {
        .source_parent_fd = parent_fd,
        .source_fd = -1,
        .source_name = name,
        .source_path = state->source.data,
        .archive_path = state->archive.data,
        .st = &status,
    };
    if (state->include_fn && !state->include_fn(&visit, state->include_user_data))
        return true;
    char* target = NULL;
    if (S_ISLNK(status.st_mode)) {
        int fd = bx_fd_openat_cloexec(parent_fd, name, O_PATH | O_NOFOLLOW, 0);
        if (fd < 0)
            return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_READLINK, errno);
        if (bx_fd_fstat_expected(fd, &status, &status) == 0)
            target = bx_path_readlinkat_dup(fd, "");
        if (!target) {
            int error = errno;
            close(fd);
            return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_READLINK, error);
        }
        visit.source_fd = fd;
    }
    visit.link_target = target;
    bool ok = state->visit_fn(&visit, state->visit_user_data, state->diag);
    if (visit.source_fd >= 0 && close(visit.source_fd) != 0 && ok)
        ok = bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_READLINK, errno);
    free(target);
    if (!ok || !recurse || !S_ISDIR(status.st_mode))
        return ok;
    DIR* dir;
    if (!bx_archive_fs_open_dir(state, parent_fd, name, &status, &dir))
        return false;
    return !dir || bx_archive_fs_visit_children(state, dir);
}

bool bx_archive_fs_visit_at_filtered(int source_parent_fd,
                                       const char* source_name,
                                       const char* source_path,
                                       const char* archive_path,
                                       bool recurse,
                                       bool sort_children,
                                       bool one_file_system,
                                       bx_archive_fs_include_fn include_fn,
                                       void* include_user_data,
                                       bx_archive_fs_error_fn error_fn,
                                       void* error_user_data,
                                       bx_archive_fs_visit_fn visit_fn,
                                       void* visit_user_data,
                                       struct bx_diag_ctx* diag) {
    struct bx_archive_fs_visit_state state = {
        .boundary = {.enabled = one_file_system},
        .sort_children = sort_children,
        .include_fn = include_fn,
        .include_user_data = include_user_data,
        .error_fn = error_fn,
        .error_user_data = error_user_data,
        .visit_fn = visit_fn,
        .visit_user_data = visit_user_data,
        .diag = diag,
    };
    bx_archive_fs_path_buf_init(&state.source, source_path);
    bx_archive_fs_path_buf_init(&state.archive, archive_path);
    bool ok = bx_archive_fs_visit_inner(&state, source_parent_fd, source_name, recurse);
    bx_archive_fs_path_buf_cleanup(&state.archive);
    bx_archive_fs_path_buf_cleanup(&state.source);
    return ok;
}

bool bx_archive_fs_add_path_filtered(struct bx_archive_fs_list* list,
                                     const char* source_path,
                                     const char* archive_path,
                                     bool recurse,
                                     bool sort_children,
                                     bx_archive_fs_include_fn include_fn,
                                     void* include_user_data,
                                     bx_archive_fs_error_fn error_fn,
                                     void* error_user_data,
                                     struct bx_diag_ctx* diag) {
    return bx_archive_fs_visit_at_filtered(AT_FDCWD,
                                             source_path,
                                             source_path,
                                             archive_path,
                                             recurse,
                                             sort_children,
                                             false,
                                             include_fn,
                                             include_user_data,
                                             error_fn,
                                             error_user_data,
                                             bx_archive_fs_collect_entry,
                                             list,
                                             diag);
}

bool bx_archive_fs_add_path(struct bx_archive_fs_list* list,
                            const char* source_path,
                            const char* archive_path,
                            bool recurse,
                            bool sort_children,
                            struct bx_diag_ctx* diag) {
    return bx_archive_fs_add_path_filtered(list,
                                           source_path,
                                           archive_path,
                                           recurse,
                                           sort_children,
                                           NULL,
                                           NULL,
                                           NULL,
                                           NULL,
                                           diag);
}

void bx_archive_pending_dirs_free(struct bx_archive_pending_dirs* dirs) {
    size_t i;
    for (i = 0u; i < dirs->len; i++) {
        bx_file_metadata_free(&dirs->entries[i].restore.metadata);
        free(dirs->entries[i].path);
    }
    free(dirs->entries);
    dirs->entries = NULL;
    dirs->len = 0u;
    dirs->cap = 0u;
}

bool bx_archive_pending_dirs_record_fd(struct bx_archive_pending_dirs* dirs,
                                    int fd,
                                    const char* path,
                                    mode_t mode,
                                    bool set_mtime,
                                    struct timespec mtime) {
    struct bx_archive_pending_dir* entry;
    struct stat status;
    int stat_rc = fstat(fd, &status);
    if (stat_rc != 0 || !S_ISDIR(status.st_mode)) {
        int error = stat_rc != 0 ? errno : ENOTDIR;
        errno = error;
        return false;
    }
    if (dirs->len == dirs->cap) {
        size_t next_cap = dirs->cap ? dirs->cap * 2u : 16u;
        dirs->entries = xrealloc(dirs->entries, next_cap * sizeof(*dirs->entries));
        dirs->cap = next_cap;
    }
    entry = &dirs->entries[dirs->len];
    memset(entry, 0, sizeof(*entry));
    entry->order = dirs->len++;
    entry->path = xstrdup(path);
    entry->dev = status.st_dev;
    entry->ino = status.st_ino;
    entry->depth = bx_dir_path_depth(path, dirs->root_depth);
    entry->restore.mode = mode;
    entry->restore.set_mode = true;
    entry->restore.mtime = mtime;
    entry->restore.set_mtime = set_mtime;
    return true;
}

static int bx_archive_pending_dir_open(int root_fd, const char* path, unsigned policy) {
    char* leaf = NULL;
    int parent = bx_dir_path_open_destination_parent(root_fd, path, policy, false, 0, &leaf);
    if (parent < 0)
        return -1;
    int fd = bx_fd_openat_cloexec(parent, leaf,
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK, 0);
    int error = errno;
    close(parent);
    free(leaf);
    errno = error;
    return fd;
}

bool bx_archive_pending_dirs_record(struct bx_archive_pending_dirs* dirs,
                                    int root_fd, const char* path, mode_t mode,
                                    bool set_mtime, struct timespec mtime) {
    int fd = bx_archive_pending_dir_open(root_fd, path, dirs->path_policy);
    if (fd < 0)
        return false;
    bool ok = bx_archive_pending_dirs_record_fd(dirs, fd, path, mode, set_mtime, mtime);
    int error = errno;
    close(fd);
    errno = error;
    return ok;
}

bool bx_archive_restore_fd(const struct bx_file_restore* restore, int fd,
                            const char* path, bool symlink, bool directory,
                            struct bx_diag_ctx* diag) {
    enum bx_file_restore_result result = bx_file_restore_fd(restore, fd, symlink, directory);
    if (result == BX_FILE_RESTORE_METADATA_ERROR)
        bx_diag(diag, "%s: cannot restore metadata: %s", path, strerror(errno));
    else if (result != BX_FILE_RESTORE_OK)
        bx_diag(diag, "%s: %s", path, strerror(errno));
    return result == BX_FILE_RESTORE_OK;
}

static int bx_archive_pending_dir_identity_compare(const void* left, const void* right) {
    const struct bx_archive_pending_dir* a = left;
    const struct bx_archive_pending_dir* b = right;
    if (a->dev != b->dev)
        return a->dev < b->dev ? -1 : 1;
    if (a->ino != b->ino)
        return a->ino < b->ino ? -1 : 1;
    return (a->order > b->order) - (a->order < b->order);
}

static int bx_archive_pending_dir_depth_compare(const void* left, const void* right) {
    const struct bx_archive_pending_dir* a = left;
    const struct bx_archive_pending_dir* b = right;
    if (a->depth != b->depth)
        return a->depth < b->depth ? -1 : 1;
    return (a->order > b->order) - (a->order < b->order);
}

bool bx_archive_pending_dirs_apply(struct bx_archive_pending_dirs* dirs,
                                   int root_fd,
                                   struct bx_diag_ctx* diag) {
    /* Collapse aliases and duplicate headers before depth sorting; keep the last. */
    if (dirs->len > 1u) {
        qsort(dirs->entries, dirs->len, sizeof(*dirs->entries), bx_archive_pending_dir_identity_compare);
        size_t kept = 0u;
        for (size_t i = 0u; i < dirs->len; i++) {
            struct bx_archive_pending_dir* entry = &dirs->entries[i];
            if (i + 1u < dirs->len && entry->dev == entry[1].dev && entry->ino == entry[1].ino) {
                bx_file_metadata_free(&entry->restore.metadata);
                free(entry->path);
            }
            else
                dirs->entries[kept++] = *entry;
        }
        dirs->len = kept;
        qsort(dirs->entries, dirs->len, sizeof(*dirs->entries), bx_archive_pending_dir_depth_compare);
    }
    while (dirs->len > 0u) {
        struct bx_archive_pending_dir* entry = &dirs->entries[dirs->len - 1u];
        int fd = bx_archive_pending_dir_open(root_fd, entry->path, dirs->path_policy);
        if (fd < 0) {
            /* A later archive member may have removed or replaced this path. */
            if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP) {
                bx_file_metadata_free(&entry->restore.metadata);
                free(entry->path);
                dirs->len--;
                continue;
            }
            bx_diag(diag, "%s: %s", entry->path, strerror(errno));
            return false;
        }
        struct stat status;
        if (fstat(fd, &status) != 0) {
            bx_diag(diag, "%s: %s", entry->path, strerror(errno));
            close(fd);
            return false;
        }
        bool same = status.st_dev == entry->dev && status.st_ino == entry->ino;
        bool ok = !same || bx_archive_restore_fd(&entry->restore, fd, entry->path, false, true, diag);
        bool closed = bx_fd_close(&fd, entry->path, diag);
        bx_file_metadata_free(&entry->restore.metadata);
        free(entry->path);
        dirs->len--;
        if (!ok || !closed)
            return false;
    }
    return true;
}
