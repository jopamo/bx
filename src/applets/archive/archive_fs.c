#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets/archive/archive_fs.h"
#include "applets/archive/archive_temp.h"
#include "bx/libbx.h"
#include "lib/dir_cycle.h"
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

/* O_DIRECTORY never opens a device or FIFO for I/O. Keep a readable
 * directory when possible; recording just its header only needs O_PATH. */
static int bx_archive_fs_select_follow_at(int start_fd, const char* path, bool directory_only) {
    int fd = bx_fd_openat_cloexec(start_fd, path, O_RDONLY | O_DIRECTORY, 0);
    return fd >= 0 ? fd : bx_fd_openat_cloexec(start_fd, path, O_PATH | (directory_only ? O_DIRECTORY : 0), 0);
}

int bx_archive_fs_open_source_dir(int parent, const char* name, bool readable) {
    if (!*name) {
        if (!readable)
            return bx_fd_dup_cloexec(parent);
        int flags = fcntl(parent, F_GETFL);
        if (flags < 0)
            return -1;
        if (!(flags & O_PATH))
            return bx_fd_dup_cloexec(parent);
        name = ".";
    }
    return bx_fd_openat_cloexec(parent, name, (readable ? O_RDONLY : O_PATH) | O_DIRECTORY | O_NOFOLLOW, 0);
}

static int bx_archive_fs_stat_source(int parent, const char* name, struct stat* status) {
    return *name ? fstatat(parent, name, status, AT_SYMLINK_NOFOLLOW) : fstat(parent, status);
}

static int bx_archive_fs_open_follow_parent_at(int start_fd, const char* path, char** leaf) {
    if (!path || !leaf) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(path);
    if (!len) {
        errno = ENOENT;
        return -1;
    }
    /* Splitting a single Linux lookup must not make an overlong operand
     * valid. Do not apply this to display paths assembled across lookups. */
    if (len >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (path[len - 1] == '/') {
        int fd = bx_archive_fs_select_follow_at(start_fd, path, true);
        if (fd >= 0)
            *leaf = xstrdup("");
        return fd;
    }
    char* directory = bx_path_dirname_dup(path);
    int fd = bx_fd_openat_cloexec(start_fd, directory, O_PATH | O_DIRECTORY, 0);
    int error = errno;
    free(directory);
    if (fd >= 0)
        *leaf = xstrdup(bx_path_basename_ptr(path));
    errno = error;
    return fd;
}

/* Let the kernel follow the link: readlink text alone does not implement
 * protected symlinks, magic links or the kernel's traversal limit. */
static int bx_archive_fs_resolve_link(int selected, int parent, const char* name, struct stat* status, char** leaf,
                                     bool* boundary_error) {
    int owned_parent = -1;
    char* owned_name = NULL;
    if (selected < 0)
        return -1;
    struct stat pinned;
    if (fstat(selected, &pinned) != 0)
        goto fail;
    if (S_ISDIR(pinned.st_mode)) {
        *status = pinned;
        *leaf = xstrdup("");
        return selected;
    }
    /* Payload and special-object metadata readers require a no-follow leaf.
     * Find a locator for the pinned inode, not an alternative to it. */
    unsigned links = 0;
    while (S_ISLNK(status->st_mode)) {
        if (++links > 40u) {
            errno = ELOOP;
            goto fail;
        }
        int fd = bx_fd_openat_cloexec(parent, name, O_PATH | O_NOFOLLOW, 0);
        if (fd < 0)
            goto fail;
        char* target = bx_fd_fstat_expected(fd, status, status) == 0
            ? bx_path_readlinkat_dup(fd, "") : NULL;
        int error = errno;
        close(fd);
        if (!target) {
            errno = error;
            goto fail;
        }
        char* next_name = NULL;
        int next_parent = bx_archive_fs_open_follow_parent_at(parent, target, &next_name);
        error = errno;
        free(target);
        if (next_parent < 0) {
            errno = error;
            goto fail;
        }
        if (owned_parent >= 0)
            close(owned_parent);
        free(owned_name);
        parent = owned_parent = next_parent;
        name = owned_name = next_name;
        if (bx_archive_fs_stat_source(parent, name, status) != 0)
            goto fail;
    }
    if (bx_fd_fstat_expected(selected, status, status) != 0)
        goto fail;
    /* Bind mounts can expose the same device/inode through different mounts. */
    struct bx_mount_identity selected_mount, named_mount;
    bool same;
    if (!bx_mount_identity_read(selected, &selected_mount)
        || !bx_mount_identity_read_at(parent, name, &named_mount)
        || !bx_mount_identity_compare(&selected_mount, &named_mount, &same)) {
        if (boundary_error)
            *boundary_error = true;
        goto fail;
    }
    if (!same || (boundary_error && !selected_mount.has_mount)) {
        if (boundary_error)
            *boundary_error = true;
        errno = !same ? ESTALE : EOPNOTSUPP;
        goto fail;
    }
    close(selected);
    *leaf = owned_name;
    return owned_parent;

fail: {
    int error = errno;
    close(selected);
    if (owned_parent >= 0)
        close(owned_parent);
    free(owned_name);
    errno = error;
    return -1;
}
}

/* The walker resolves the explicit root in one lookup. Each child directory
 * is a separate lookup, even if the resulting display path exceeds PATH_MAX
 * or contains more links than one kernel pathname lookup permits. */
static int bx_archive_fs_open_descendant_parent(int root_fd, const char* path, size_t operand_len, char** leaf) {
    char* copy = xstrdup(path);
    char* child = copy + operand_len;
    char saved = *child;
    *child = '\0';
    int parent = bx_fd_openat_cloexec(root_fd, copy, O_PATH | O_DIRECTORY, 0);
    *child = saved;
    if (*child == '/')
        child++;
    while (parent >= 0) {
        char* slash = strchr(child, '/');
        if (!slash) {
            *leaf = xstrdup(child);
            break;
        }
        *slash = '\0';
        int next = bx_fd_openat_cloexec(parent, child, O_PATH | O_DIRECTORY, 0);
        int error = errno;
        close(parent);
        errno = error;
        parent = next;
        child = slash + 1;
    }
    int error = errno;
    free(copy);
    errno = error;
    return parent;
}

bool bx_archive_fs_follow_chdir(int* cwd_fd, const char* path, struct bx_diag_ctx* diag) {
    int next = bx_fd_openat_cloexec(*cwd_fd, path, O_PATH | O_DIRECTORY, 0);
    if (next < 0 || faccessat(next, ".", X_OK, AT_EACCESS) != 0) {
        bx_diag(diag, "%s: cannot change directory: %s", path, strerror(errno));
        if (next >= 0)
            close(next);
        return false;
    }
    if (*cwd_fd >= 0)
        close(*cwd_fd);
    *cwd_fd = next;
    return true;
}

int bx_archive_fs_reopen_source(int root_fd, const struct bx_archive_fs_entry* entry, char** leaf) {
    if (!entry->follow_operand_len)
        return bx_dir_path_open_source_parent(entry->source_path, leaf);

    char* name = NULL;
    const char* path = entry->source_path + entry->source_offset;
    bool operand = entry->source_path[entry->follow_operand_len] == '\0';
    int parent = operand ? bx_archive_fs_open_follow_parent_at(root_fd, path, &name)
                         : bx_archive_fs_open_descendant_parent(root_fd, path, entry->follow_operand_len - entry->source_offset, &name);
    if (parent < 0)
        return -1;
    struct stat status;
    int target = -1;
    if (bx_archive_fs_stat_source(parent, name, &status) == 0) {
        if (!S_ISLNK(status.st_mode)) {
            *leaf = name;
            return parent;
        }
        int selected = bx_archive_fs_select_follow_at(operand ? root_fd : parent, operand ? path : name, false);
        target = bx_archive_fs_resolve_link(selected, parent, name, &status, leaf, NULL);
    }
    int error = errno;
    close(parent);
    free(name);
    errno = error;
    return target;
}

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
    bx_archive_name_list_free(&list->chdirs);
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
    struct stat parent;
    if (fstat(entry->source_parent_fd, &parent) != 0) {
        bx_diag(diag, "%s: cannot inspect source parent: %s", entry->source_path, strerror(errno));
        return false;
    }
    if (!bx_archive_fs_list_push(list, entry->source_path, entry->archive_path, entry->st, entry->link_target))
        return false;
    list->entries[list->len - 1].source_parent_dev = parent.st_dev;
    list->entries[list->len - 1].source_parent_ino = parent.st_ino;
    return true;
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
    struct bx_dir_stack* ancestor;
    int root_fd;
    const char* operand;
    size_t operand_len;
    bool sort_children;
    bool dereference;
    bx_archive_fs_include_name_fn include_name;
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

static bool bx_archive_fs_mount_error(struct bx_archive_fs_visit_state* state, int error) {
    bx_diag(state->diag, "%s: --one-file-system mount identity: %s", state->source.data, strerror(error));
    return false;
}

static bool bx_archive_fs_check_boundary(struct bx_archive_fs_visit_state* state, int parent, const char* name,
                                         struct bx_mount_identity* identity, bool* same) {
    bool ok;
    if (!*name)
        ok = bx_mount_identity_read(parent, identity);
    else if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        int fd = bx_fd_openat_cloexec(parent, name, O_PATH | O_DIRECTORY | O_NOFOLLOW, 0);
        if (fd < 0)
            return bx_archive_fs_mount_error(state, errno);
        ok = bx_mount_identity_read(fd, identity);
        int error = errno;
        if (close(fd) != 0 && ok) {
            ok = false;
            error = errno;
        }
        errno = error;
    }
    else
        ok = bx_mount_identity_read_at(parent, name, identity);
    if (!ok)
        return bx_archive_fs_mount_error(state, errno);
    if (!identity->has_mount)
        return bx_archive_fs_mount_error(state, EOPNOTSUPP);
    if (!state->boundary.initialized) {
        state->boundary.root = *identity;
        state->boundary.initialized = true;
        *same = true;
        return true;
    }
    if (!bx_mount_identity_compare(&state->boundary.root, identity, same))
        return bx_archive_fs_mount_error(state, errno);
    return true;
}

/* A NULL result means the caller must not descend (boundary or skipped error).
 * Keep the checked directory open through child lookup, including sorted walks. */
static bool bx_archive_fs_open_dir(struct bx_archive_fs_visit_state* state, int parent_fd, const char* name, const struct stat* expected, DIR** result) {
    *result = NULL;
    int fd = bx_archive_fs_open_source_dir(parent_fd, name, true);
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
        if (ok && !identity.has_mount) {
            errno = EOPNOTSUPP;
            ok = false;
        }
        if (ok)
            ok = bx_mount_identity_compare(&state->boundary.root, &identity, &same);
        if (!ok || !same) {
            int error = errno;
            int rc = close(fd);
            if (!ok)
                return bx_archive_fs_mount_error(state, error);
            if (rc != 0)
                return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_CLOSEDIR, errno);
            return true;
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
        if (!state->dereference || !state->include_name ||
            state->include_name(state->source.data, state->archive.data, state->include_user_data))
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

static bool bx_archive_fs_visit_resolved(struct bx_archive_fs_visit_state* state, int parent_fd,
                                         const char* name, struct stat status, bool recurse) {
    struct bx_mount_identity mount = {0};
    bool same_mount = true;
    if (state->boundary.enabled) {
        if (!bx_archive_fs_check_boundary(state, parent_fd, name, &mount, &same_mount))
            return false;
        if (!same_mount) {
            if (!S_ISDIR(status.st_mode))
                return true;
            recurse = false;
        }
    }
    struct bx_archive_fs_visit_entry visit = {
        .source_parent_fd = parent_fd,
        .source_fd = -1,
        .source_name = name,
        .source_path = state->source.data,
        .archive_path = state->archive.data,
        .st = &status,
        .follow_operand_len = state->dereference ? state->operand_len : 0,
        .source_mount = mount,
        .mount_boundary = !same_mount,
    };
    if (!state->dereference && state->include_name &&
        !state->include_name(visit.source_path, visit.archive_path, state->include_user_data))
        return true;
    if (state->include_fn && !state->include_fn(&visit, state->include_user_data))
        return true;
    if (state->dereference && recurse && S_ISDIR(status.st_mode)
        && bx_dir_stack_contains(state->ancestor, &status))
        return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_OPENDIR, ELOOP);
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
    if (!dir)
        return true;
    if (!state->dereference)
        return bx_archive_fs_visit_children(state, dir);
    struct bx_dir_stack ancestor = {
        .dev = status.st_dev, .ino = status.st_ino, .parent = state->ancestor,
    };
    state->ancestor = &ancestor;
    ok = bx_archive_fs_visit_children(state, dir);
    state->ancestor = ancestor.parent;
    return ok;
}

static bool bx_archive_fs_visit_inner(struct bx_archive_fs_visit_state* state, int parent_fd,
                                      const char* name, bool recurse) {
    struct stat status;
    if (bx_archive_fs_stat_source(parent_fd, name, &status) != 0)
        return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_LSTAT, errno);
    if (!state->dereference || !S_ISLNK(status.st_mode))
        return bx_archive_fs_visit_resolved(state, parent_fd, name, status, recurse);

    char* leaf = NULL;
    struct stat resolved = status;
    bool boundary_error = false;
    bool operand = state->source.len == state->operand_len;
    int selected = bx_archive_fs_select_follow_at(operand ? state->root_fd : parent_fd, operand ? state->operand : name, false);
    int target = bx_archive_fs_resolve_link(selected, parent_fd, name, &resolved, &leaf,
                                             state->boundary.enabled ? &boundary_error : NULL);
    if (target < 0) {
        int error = errno;
        if (boundary_error)
            return bx_archive_fs_mount_error(state, error);
        return bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_LSTAT, error);
    }
    bool ok = bx_archive_fs_visit_resolved(state, target, leaf, resolved, recurse);
    if (close(target) != 0 && ok)
        ok = bx_archive_fs_visit_error(state, BX_ARCHIVE_FS_ERROR_CLOSEDIR, errno);
    free(leaf);
    return ok;
}

bool bx_archive_fs_visit_at_filtered(int source_root_fd,
                                       const char* operand,
                                       const char* source_path,
                                       const char* archive_path,
                                       bool recurse,
                                       bool sort_children,
                                       bool one_file_system,
                                       bool dereference,
                                       bx_archive_fs_include_name_fn include_name,
                                       bx_archive_fs_include_fn include_fn,
                                       void* include_user_data,
                                       bx_archive_fs_error_fn error_fn,
                                       void* error_user_data,
                                       bx_archive_fs_visit_fn visit_fn,
                                       void* visit_user_data,
                                       struct bx_diag_ctx* diag) {
    char* leaf = NULL;
    if (dereference && include_name && !include_name(source_path, archive_path, include_user_data))
        return true;
    int parent = dereference ? bx_archive_fs_open_follow_parent_at(source_root_fd, operand, &leaf)
                             : bx_dir_path_open_source_parent_at(source_root_fd, operand, &leaf);
    if (parent < 0)
        return bx_archive_fs_handle_error(source_path, BX_ARCHIVE_FS_ERROR_LSTAT, errno, error_fn, error_user_data, diag) == BX_ARCHIVE_FS_ERROR_SKIP;
    struct bx_archive_fs_visit_state state = {
        .boundary = {.enabled = one_file_system},
        .root_fd = source_root_fd,
        .operand = operand,
        .sort_children = sort_children,
        .dereference = dereference,
        .include_name = include_name,
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
    state.operand_len = state.source.len;
    bool ok = bx_archive_fs_visit_inner(&state, parent, leaf, recurse);
    if (close(parent) != 0 && ok)
        ok = bx_archive_fs_visit_error(&state, BX_ARCHIVE_FS_ERROR_CLOSEDIR, errno);
    free(leaf);
    bx_archive_fs_path_buf_cleanup(&state.archive);
    bx_archive_fs_path_buf_cleanup(&state.source);
    return ok;
}

bool bx_archive_fs_add_path_filtered(struct bx_archive_fs_list* list,
                                     int source_root_fd,
                                     const char* source_path,
                                     const char* archive_path,
                                     bool recurse,
                                     bool sort_children,
                                     bx_archive_fs_include_fn include_fn,
                                     void* include_user_data,
                                     bx_archive_fs_error_fn error_fn,
                                     void* error_user_data,
                                     struct bx_diag_ctx* diag) {
    return bx_archive_fs_visit_at_filtered(source_root_fd, source_path, source_path, archive_path, recurse, sort_children, false, false, NULL, include_fn, include_user_data, error_fn, error_user_data,
                                              bx_archive_fs_collect_entry, list, diag);
}

bool bx_archive_fs_add_path(struct bx_archive_fs_list* list,
                            const char* source_path,
                            const char* archive_path,
                            bool recurse,
                            bool sort_children,
                            struct bx_diag_ctx* diag) {
    return bx_archive_fs_add_path_filtered(list, AT_FDCWD, source_path, archive_path, recurse, sort_children, NULL, NULL, NULL, NULL, diag);
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
    if (dirs->len >= BX_ARCHIVE_PENDING_METADATA_LIMIT || oversized || bytes > 256u * 1024u * 1024u - dirs->bytes) {
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
int bx_archive_pending_metadata_open_fd(int root_fd, unsigned policy, const struct bx_archive_pending_metadata_entry* expected, bool writable) {
    if ((expected->type != S_IFREG && expected->type != S_IFDIR) || (writable && expected->type != S_IFREG)) {
        errno = EINVAL;
        return -1;
    }
    char* leaf = NULL;
    int parent = bx_dir_path_open_destination_parent_from(root_fd, expected->path, expected->boundary_prefix, policy, false, 0, &leaf);
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
    int flags = (writable ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_NONBLOCK;
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
                int fd = bx_archive_pending_metadata_open_fd(root_fd, dirs->path_policy, entry, false);
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
        int fd = bx_archive_pending_metadata_open_fd(root_fd, dirs->path_policy, location, false);
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
