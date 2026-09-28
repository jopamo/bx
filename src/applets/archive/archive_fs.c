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
    if (state->include_fn && !state->include_fn(state->source.data, state->archive.data,
                                                &status, state->include_user_data))
        return true;
    char* target = NULL;
    if (S_ISLNK(status.st_mode)) {
        int fd = bx_fd_openat_cloexec(parent_fd, name, O_PATH | O_NOFOLLOW, 0);
        if (fd < 0)
            return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_READLINK, errno);
        if (bx_fd_fstat_expected(fd, &status, &status) == 0)
            target = bx_path_readlinkat_dup(fd, "");
        int error = errno;
        int rc = close(fd);
        if (!target)
            return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_READLINK, error);
        if (rc != 0) {
            free(target);
            return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_READLINK, errno);
        }
    }
    bool ok = state->visit_fn(&(struct bx_archive_fs_visit_entry){
        .source_parent_fd = parent_fd,
        .source_name = name,
        .source_path = state->source.data,
        .archive_path = state->archive.data,
        .st = &status,
        .link_target = target,
    }, state->visit_user_data, state->diag);
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

void bx_archive_parent_dir_cache_cleanup(struct bx_archive_parent_dir_cache* cache) {
    if (cache == NULL) {
        return;
    }
    free(cache->last_parent);
    cache->last_parent = NULL;
    cache->last_parent_len = 0u;
}

void bx_archive_parent_dir_cache_invalidate(struct bx_archive_parent_dir_cache* cache) {
    bx_archive_parent_dir_cache_cleanup(cache);
}

bool bx_archive_parent_dir_cache_matches_parent(const struct bx_archive_parent_dir_cache* cache,
                                                const char* parent) {
    return cache != NULL
        && cache->last_parent != NULL
        && parent != NULL
        && cache->last_parent_len == strlen(parent)
        && strcmp(cache->last_parent, parent) == 0;
}

void bx_archive_parent_dir_cache_remember_parent(struct bx_archive_parent_dir_cache* cache,
                                                 const char* parent) {
    if (cache == NULL) {
        return;
    }

    free(cache->last_parent);
    cache->last_parent = xstrdup(parent);
    cache->last_parent_len = strlen(parent);
}

static bool
bx_archive_parent_dir_cache_matches_path_parent(const struct bx_archive_parent_dir_cache* cache,
                                                const char* path) {
    const char* slash;

    if (cache == NULL || cache->last_parent == NULL || path == NULL) {
        return false;
    }

    slash = strrchr(path, '/');
    if (slash == NULL) {
        return cache->last_parent_len == 1u && cache->last_parent[0] == '.';
    }
    if (slash == path) {
        return cache->last_parent_len == 1u && cache->last_parent[0] == '/';
    }

    return cache->last_parent_len == (size_t)(slash - path)
        && memcmp(cache->last_parent, path, cache->last_parent_len) == 0;
}

bool bx_archive_remove_path_tree(const char* path, struct bx_diag_ctx* diag) {
    struct stat st;

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        if (unlink(path) != 0) {
            bx_diag(diag, "%s: %s", path, strerror(errno));
            return false;
        }
        return true;
    }

    {
        DIR* dir = opendir(path);
        struct dirent* ent;

        if (dir == NULL) {
            bx_diag(diag, "%s: %s", path, strerror(errno));
            return false;
        }
        while ((ent = readdir(dir)) != NULL) {
            char* child_path;
            bool ok;

            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            child_path = bx_path_join(path, ent->d_name);
            ok = bx_archive_remove_path_tree(child_path, diag);
            free(child_path);
            if (!ok) {
                closedir(dir);
                return false;
            }
        }
        closedir(dir);
    }

    if (rmdir(path) != 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

static bool bx_archive_ensure_parent_dirs_impl(const char* path,
                                               struct bx_archive_parent_dir_cache* cache,
                                               bool safe_existing,
                                               struct bx_diag_ctx* diag) {
    char* parent;
    char* cursor;
    size_t i;

    if (bx_archive_parent_dir_cache_matches_path_parent(cache, path)) {
        return true;
    }

    parent = bx_path_parent_dir_dup(path);
    if (parent == NULL) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    if (strcmp(parent, ".") == 0 || strcmp(parent, "/") == 0) {
        free(parent);
        return true;
    }

    cursor = xstrdup(parent);

    if (cursor[0] == '/') {
        i = 1u;
    }
    else {
        i = 0u;
    }

    for (; cursor[i] != '\0'; i++) {
        struct stat st;

        if (cursor[i] != '/') {
            continue;
        }
        cursor[i] = '\0';
        if (cursor[0] == '\0') {
            cursor[i] = '/';
            continue;
        }
        if (!safe_existing) {
            if (mkdir(cursor, 0777u) != 0 && errno != EEXIST) {
                bx_diag(diag, "%s: %s", cursor, strerror(errno));
                free(parent);
                free(cursor);
                return false;
            }
            cursor[i] = '/';
            continue;
        }
        if (lstat(cursor, &st) != 0) {
            if (errno != ENOENT) {
                bx_diag(diag, "%s: %s", cursor, strerror(errno));
                free(parent);
                free(cursor);
                return false;
            }
            if (mkdir(cursor, 0777u) != 0) {
                bx_diag(diag, "%s: %s", cursor, strerror(errno));
                free(parent);
                free(cursor);
                return false;
            }
        }
        else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            bx_archive_parent_dir_cache_invalidate(cache);
            if (!bx_archive_remove_path_tree(cursor, diag) || mkdir(cursor, 0777u) != 0) {
                if (errno != 0 && !S_ISDIR(st.st_mode)) {
                    bx_diag(diag, "%s: %s", cursor, strerror(errno));
                }
                free(parent);
                free(cursor);
                return false;
            }
        }
        cursor[i] = '/';
    }

    if (safe_existing) {
        struct stat st;

        if (lstat(cursor, &st) != 0) {
            if (errno != ENOENT) {
                bx_diag(diag, "%s: %s", cursor, strerror(errno));
                free(parent);
                free(cursor);
                return false;
            }
            if (mkdir(cursor, 0777u) != 0) {
                bx_diag(diag, "%s: %s", cursor, strerror(errno));
                free(parent);
                free(cursor);
                return false;
            }
        }
        else if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            bx_archive_parent_dir_cache_invalidate(cache);
            if (!bx_archive_remove_path_tree(cursor, diag) || mkdir(cursor, 0777u) != 0) {
                if (errno != 0 && !S_ISDIR(st.st_mode)) {
                    bx_diag(diag, "%s: %s", cursor, strerror(errno));
                }
                free(parent);
                free(cursor);
                return false;
            }
        }
    }
    else if (mkdir(cursor, 0777u) != 0 && errno != EEXIST) {
        bx_diag(diag, "%s: %s", cursor, strerror(errno));
        free(parent);
        free(cursor);
        return false;
    }

    bx_archive_parent_dir_cache_remember_parent(cache, parent);
    free(parent);
    free(cursor);
    return true;
}

bool bx_archive_ensure_parent_dirs_cached(const char* path,
                                          struct bx_archive_parent_dir_cache* cache,
                                          struct bx_diag_ctx* diag) {
    return bx_archive_ensure_parent_dirs_impl(path, cache, false, diag);
}

bool bx_archive_ensure_parent_dirs(const char* path, struct bx_diag_ctx* diag) {
    return bx_archive_ensure_parent_dirs_cached(path, NULL, diag);
}

bool bx_archive_ensure_parent_dirs_safe_cached(const char* path,
                                               struct bx_archive_parent_dir_cache* cache,
                                               struct bx_diag_ctx* diag) {
    return bx_archive_ensure_parent_dirs_impl(path, cache, true, diag);
}

bool bx_archive_ensure_parent_dirs_safe(const char* path, struct bx_diag_ctx* diag) {
    return bx_archive_ensure_parent_dirs_safe_cached(path, NULL, diag);
}

void bx_archive_pending_dirs_free(struct bx_archive_pending_dirs* dirs) {
    size_t i;
    for (i = 0u; i < dirs->len; i++) {
        bx_file_metadata_free(&dirs->entries[i].metadata);
        close(dirs->entries[i].fd);
        free(dirs->entries[i].path);
    }
    free(dirs->entries);
    dirs->entries = NULL;
    dirs->len = 0u;
    dirs->cap = 0u;
}

static bool bx_archive_pending_dirs_record_owned(struct bx_archive_pending_dirs* dirs,
                                    int fd,
                                    const char* path,
                                    mode_t mode,
                                    bool set_mtime,
                                    struct timespec mtime) {
    struct bx_archive_pending_dir* entry;
    struct stat status;
    if (fd < 0)
        return false;
    int stat_rc = fstat(fd, &status);
    if (stat_rc != 0 || !S_ISDIR(status.st_mode)) {
        int error = stat_rc != 0 ? errno : ENOTDIR;
        close(fd);
        errno = error;
        return false;
    }
    if (dirs->len == dirs->cap) {
        size_t next_cap = dirs->cap ? dirs->cap * 2u : 16u;
        dirs->entries = xrealloc(dirs->entries, next_cap * sizeof(*dirs->entries));
        dirs->cap = next_cap;
    }
    entry = &dirs->entries[dirs->len++];
    memset(entry, 0, sizeof(*entry));
    entry->path = xstrdup(path);
    entry->fd = fd;
    entry->mode = mode;
    entry->mtime = mtime;
    entry->set_mtime = set_mtime;
    return true;
}

bool bx_archive_pending_dirs_record(struct bx_archive_pending_dirs* dirs,
                                    const char* path, mode_t mode,
                                    bool set_mtime, struct timespec mtime) {
    int fd = bx_fd_open_cloexec(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK, 0);
    return bx_archive_pending_dirs_record_owned(dirs, fd, path, mode, set_mtime, mtime);
}

bool bx_archive_pending_dirs_record_fd(struct bx_archive_pending_dirs* dirs,
                                       int fd, const char* path, mode_t mode,
                                       bool set_mtime, struct timespec mtime) {
    return bx_archive_pending_dirs_record_owned(dirs, bx_fd_dup_cloexec(fd), path, mode, set_mtime, mtime);
}

bool bx_archive_set_path_mtime(const char* path,
                               struct timespec mtime,
                               bool nofollow,
                               struct bx_diag_ctx* diag) {
    struct timespec times[2];
    int flags = nofollow ? AT_SYMLINK_NOFOLLOW : 0;

    times[0] = mtime;
    times[1] = mtime;
    if (utimensat(AT_FDCWD, path, times, flags) != 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

bool bx_archive_set_fd_mtime(int fd,
                             const char* path,
                             struct timespec mtime,
                             struct bx_diag_ctx* diag) {
    struct timespec times[2];

    times[0] = mtime;
    times[1] = mtime;
    if (futimens(fd, times) != 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

bool bx_archive_pending_dirs_apply(struct bx_archive_pending_dirs* dirs,
                                   struct bx_diag_ctx* diag) {
    while (dirs->len > 0u) {
        struct bx_archive_pending_dir* entry = &dirs->entries[dirs->len - 1u];
        if (bx_fd_fchmod(entry->fd, entry->mode & 07777u) != 0) {
            bx_diag(diag, "%s: %s", entry->path, strerror(errno));
            return false;
        }
        if (!bx_file_metadata_apply(&entry->metadata, entry->fd, entry->path,
                                     false, true, entry->mode)) {
            bx_diag(diag, "%s: cannot restore metadata: %s", entry->path, strerror(errno));
            return false;
        }
        if (entry->set_mtime && !bx_archive_set_fd_mtime(entry->fd, entry->path, entry->mtime, diag)) {
            return false;
        }
        bool closed = bx_fd_close(&entry->fd, entry->path, diag);
        bx_file_metadata_free(&entry->metadata);
        free(entry->path);
        dirs->len--;
        if (!closed)
            return false;
    }
    return true;
}
