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
#include "applets/archive/archive_temp.h"
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

void bx_archive_pending_metadata_free(struct bx_archive_pending_metadata* dirs) {
    size_t i;
    for (i = 0u; i < dirs->len; i++) {
        bx_file_metadata_free(&dirs->entries[i].restore.metadata);
        free(dirs->entries[i].path);
    }
    free(dirs->entries);
    dirs->entries = NULL;
    dirs->len = 0u;
    dirs->cap = 0u;
    dirs->bytes = 0u;
}

static bool bx_archive_pending_metadata_record(struct bx_archive_pending_metadata* dirs,
                                               int fd,
                                               const char* path,
                                               size_t boundary_prefix,
                                               const struct bx_file_restore* restore,
                                               uint64_t order,
                                               uint64_t origin,
                                               bool alias) {
    struct bx_archive_pending_metadata_entry* entry;
    struct stat status;
    int stat_rc = fstat(fd, &status);
    if (stat_rc != 0 || (!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode)) || (alias && !S_ISREG(status.st_mode)) || origin > order || !path || boundary_prefix > strlen(path) ||
        (boundary_prefix && path[boundary_prefix - 1] != '/')) {
        int error = stat_rc != 0 ? errno : EINVAL;
        errno = error;
        return false;
    }
    size_t bytes = sizeof(*entry) + strlen(path) + 1u;
    const struct bx_file_metadata* metadata = &restore->metadata;
    size_t values = bx_file_metadata_value_bytes(metadata);
    const size_t limit = 256u * 1024u * 1024u;
    bool oversized = bytes > limit || metadata->len > limit / sizeof(*metadata->xattrs);
    if (!oversized) {
        size_t slots = metadata->len * sizeof(*metadata->xattrs);
        oversized = slots > limit - bytes || values > limit - bytes - slots;
        if (!oversized)
            bytes += slots + values;
    }
    if (dirs->len >= 1048576u || oversized || bytes > 256u * 1024u * 1024u - dirs->bytes) {
        errno = E2BIG;
        return false;
    }
    if (dirs->len == dirs->cap) {
        size_t next_cap = dirs->cap ? dirs->cap * 2u : 16u;
        dirs->entries = xrealloc(dirs->entries, next_cap * sizeof(*dirs->entries));
        dirs->cap = next_cap;
    }
    entry = &dirs->entries[dirs->len];
    memset(entry, 0, sizeof(*entry));
    entry->order = order;
    entry->origin = origin;
    entry->alias = alias;
    dirs->len++;
    entry->path = xstrdup(path);
    entry->boundary_prefix = boundary_prefix;
    entry->dev = status.st_dev;
    entry->ino = status.st_ino;
    entry->type = status.st_mode & S_IFMT;
    entry->depth = bx_dir_path_depth(path, dirs->root_depth);
    entry->restore = *restore;
    entry->restore.metadata = (struct bx_file_metadata){0};
    bx_file_metadata_copy(&entry->restore.metadata, metadata);
    dirs->bytes += bytes;
    return true;
}

bool bx_archive_pending_metadata_record_fd(struct bx_archive_pending_metadata* dirs,
                                           int fd,
                                           const char* path,
                                           size_t boundary_prefix,
                                           const struct bx_file_restore* restore,
                                           uint64_t order,
                                           uint64_t origin) {
    return bx_archive_pending_metadata_record(dirs, fd, path, boundary_prefix, restore, order, origin, false);
}

bool bx_archive_pending_metadata_record_alias_fd(struct bx_archive_pending_metadata* dirs, int fd, const char* path, size_t boundary_prefix, uint64_t origin) {
    const struct bx_file_restore restore = {0};
    return bx_archive_pending_metadata_record(dirs, fd, path, boundary_prefix, &restore, origin, origin, true);
}

/* -2 means the saved object no longer occupies this approved location. */
static int bx_archive_pending_metadata_entry_open(int root_fd, const char* path, unsigned policy, const struct bx_archive_pending_metadata_entry* expected) {
    char* leaf = NULL;
    int parent = bx_dir_path_open_destination_parent_from(root_fd, path, expected->boundary_prefix, policy, false, 0, &leaf);
    if (parent < 0)
        return (errno == ENOENT || errno == ENOTDIR || errno == ELOOP) ? -2 : -1;
    int fd = -1;
    struct stat status;
    {
        int locator = bx_fd_openat_cloexec(parent, leaf, O_PATH | O_NOFOLLOW, 0);
        bool missing = locator < 0 && (errno == ENOENT || errno == ENOTDIR || errno == ELOOP);
        bool found = locator >= 0 && fstat(locator, &status) == 0;
        int error = errno;
        if (locator >= 0)
            close(locator);
        errno = error;
        if (missing || (found && (status.st_dev != expected->dev || status.st_ino != expected->ino || (status.st_mode & S_IFMT) != expected->type))) {
            fd = -2;
            goto done;
        }
        if (!found)
            goto done;
    }
    int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
    if (expected->type == S_IFDIR)
        flags |= O_DIRECTORY;
    fd = bx_fd_openat_cloexec(parent, leaf, flags, 0);
    if (fd >= 0) {
        struct stat opened;
        if (bx_fd_fstat_expected(fd, &status, &opened) != 0) {
            int error = errno;
            close(fd);
            fd = error == ESTALE ? -2 : -1;
            errno = error;
        }
    }
done: {
    int error = errno;
    close(parent);
    free(leaf);
    errno = error;
    return fd;
}
}

static bool bx_archive_restore_result(enum bx_file_restore_result result, const char* path, struct bx_diag_ctx* diag) {
    if (result == BX_FILE_RESTORE_METADATA_ERROR)
        bx_diag(diag, "%s: cannot restore metadata: %s", path, strerror(errno));
    else if (result != BX_FILE_RESTORE_OK)
        bx_diag(diag, "%s: %s", path, strerror(errno));
    return result == BX_FILE_RESTORE_OK;
}

bool bx_archive_restore_fd(const struct bx_file_restore* restore, int fd, const char* path, struct bx_diag_ctx* diag) {
    return bx_archive_restore_result(bx_file_restore_fd(restore, fd), path, diag);
}

bool bx_archive_prepare_regular_fd(const struct bx_file_restore* restore, int fd, const char* path, struct bx_diag_ctx* diag) {
    return bx_archive_restore_result(bx_file_restore_prepare_regular(restore, fd), path, diag);
}

bool bx_archive_restore_leaf(const struct bx_file_restore* restore, int parent_fd, const char* leaf, const struct stat* expected, const char* path, struct bx_diag_ctx* diag) {
    struct bx_file_metadata_target target;
    enum bx_file_restore_result result = bx_file_metadata_target_leaf(&target, parent_fd, leaf, expected) ? bx_file_restore_target(restore, &target) : BX_FILE_RESTORE_STAT_ERROR;
    return bx_archive_restore_result(result, path, diag);
}

static int bx_archive_pending_metadata_entry_identity_compare(const void* left, const void* right) {
    const struct bx_archive_pending_metadata_entry* a = left;
    const struct bx_archive_pending_metadata_entry* b = right;
    if (a->dev != b->dev)
        return a->dev < b->dev ? -1 : 1;
    if (a->ino != b->ino)
        return a->ino < b->ino ? -1 : 1;
    if (a->type != b->type)
        return a->type < b->type ? -1 : 1;
    return (a->order > b->order) - (a->order < b->order);
}

struct bx_archive_pending_metadata_group {
    size_t first;
    size_t end;
    size_t location;
    ptrdiff_t depth;
};

static int bx_archive_pending_metadata_entry_depth_compare(const void* left, const void* right) {
    const struct bx_archive_pending_metadata_group* a = left;
    const struct bx_archive_pending_metadata_group* b = right;
    if (a->depth != b->depth)
        return a->depth < b->depth ? -1 : 1;
    return (a->first > b->first) - (a->first < b->first);
}

bool bx_archive_pending_metadata_apply(struct bx_archive_pending_metadata* dirs, int root_fd, struct bx_diag_ctx* diag) {
    if (!dirs->len)
        return true;
    qsort(dirs->entries, dirs->len, sizeof(*dirs->entries), bx_archive_pending_metadata_entry_identity_compare);
    struct bx_archive_pending_metadata_group* groups = xmalloc(dirs->len * sizeof(*groups));
    size_t count = 0;
    bool ok = true;
    for (size_t first = 0; first < dirs->len;) {
        size_t end = first + 1u;
        while (end < dirs->len && dirs->entries[end].dev == dirs->entries[first].dev && dirs->entries[end].ino == dirs->entries[first].ino && dirs->entries[end].type == dirs->entries[first].type)
            end++;
        size_t location = first;
        bool live = end == first + 1u;
        /* Choose a live, deepest locator before restrictive ancestor metadata. */
        if (!live) {
            for (size_t i = first; i < end; i++) {
                if (bx_archive_temp_pending_signal()) {
                    bx_diag(diag, "metadata finalization interrupted");
                    ok = false;
                    goto done;
                }
                struct bx_archive_pending_metadata_entry* entry = &dirs->entries[i];
                int fd = bx_archive_pending_metadata_entry_open(root_fd, entry->path, dirs->path_policy, entry);
                if (fd == -2)
                    continue;
                if (fd < 0) {
                    bx_diag(diag, "%s: %s", entry->path, strerror(errno));
                    ok = false;
                    goto done;
                }
                if (!live || entry->depth >= dirs->entries[location].depth)
                    location = i;
                live = true;
                if (!bx_fd_close(&fd, entry->path, diag)) {
                    ok = false;
                    goto done;
                }
            }
        }
        if (live)
            groups[count++] = (struct bx_archive_pending_metadata_group){first, end, location, dirs->entries[location].depth};
        first = end;
    }
    qsort(groups, count, sizeof(*groups), bx_archive_pending_metadata_entry_depth_compare);
    while (count) {
        if (bx_archive_temp_pending_signal()) {
            bx_diag(diag, "metadata finalization interrupted");
            ok = false;
            goto done;
        }
        struct bx_archive_pending_metadata_group* group = &groups[--count];
        struct bx_archive_pending_metadata_entry* location = &dirs->entries[group->location];
        int fd = bx_archive_pending_metadata_entry_open(root_fd, location->path, dirs->path_policy, location);
        if (fd == -2)
            continue;
        if (fd < 0) {
            bx_diag(diag, "%s: %s", location->path, strerror(errno));
            ok = false;
            goto done;
        }
        bool directory = location->type == S_IFDIR;
        size_t first = directory ? group->end - 1u : group->first;
        if (!directory) {
            uint64_t origin = 0u;
            for (size_t i = first; i < group->end; i++)
                if (dirs->entries[i].origin > origin)
                    origin = dirs->entries[i].origin;
            while (first < group->end && dirs->entries[first].order < origin)
                first++;
        }
        unsigned int set = 0u, clear = 0u;
        const struct bx_file_xattr* label = NULL;
        const struct bx_file_xattr* capabilities = NULL;
        struct bx_file_restore final = {0};
        for (size_t i = first; ok && i < group->end; i++) {
            if (bx_archive_temp_pending_signal()) {
                bx_diag(diag, "metadata finalization interrupted");
                ok = false;
                break;
            }
            if (dirs->entries[i].alias)
                continue;
            struct bx_file_restore restore = dirs->entries[i].restore;
            if (restore.flags_present) {
                if (!(restore.flags_set | restore.flags_clear))
                    set = clear = 0u;
                else {
                    set = (set & ~restore.flags_clear) | restore.flags_set;
                    clear = (clear & ~restore.flags_set) | restore.flags_clear;
                }
            }
            restore.flags_set = restore.flags_clear = 0u;
            for (size_t attr = 0; attr < restore.metadata.len; attr++) {
                const struct bx_file_xattr* value = &restore.metadata.xattrs[attr];
                enum bx_file_xattr_class kind = bx_file_xattr_classify(value->name);
                if (kind == BX_FILE_XATTR_SELINUX)
                    label = value;
                else if (kind == BX_FILE_XATTR_CAPABILITY)
                    capabilities = value;
            }
            if (restore.set_mtime) {
                final.set_mtime = true;
                final.mtime = restore.mtime;
            }
            ok = bx_archive_restore_result(bx_file_restore_fd_base(&restore, fd), location->path, diag);
        }
        if (ok && bx_archive_temp_pending_signal()) {
            bx_diag(diag, "metadata finalization interrupted");
            ok = false;
        }
        if (ok) {
            struct bx_file_xattr attrs[2];
            if (label)
                attrs[final.metadata.len++] = *label;
            if (capabilities)
                attrs[final.metadata.len++] = *capabilities;
            final.metadata.xattrs = attrs;
            ok = bx_archive_restore_fd(&final, fd, location->path, diag);
        }
        if (ok && !bx_file_metadata_apply_flags(fd, set, clear)) {
            bx_diag(diag, "%s: cannot restore file flags: %s", location->path, strerror(errno));
            ok = false;
        }
        if (!bx_fd_close(&fd, location->path, diag))
            ok = false;
        if (!ok)
            goto done;
    }
done:
    free(groups);
    if (ok)
        bx_archive_pending_metadata_free(dirs);
    return ok;
}
