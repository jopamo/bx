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
#include "lib/path_ops.h"

struct bx_archive_fs_path_buf {
    char* data;
    size_t len;
    size_t cap;
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

static bool bx_archive_read_children(const char* dir_path,
                                     bool sort_children,
                                     char*** names_out,
                                     size_t* count_out,
                                     bx_archive_fs_error_fn error_fn,
                                     void* error_user_data,
                                     struct bx_diag_ctx* diag) {
    DIR* dir = opendir(dir_path);
    struct dirent* ent;
    char** names = NULL;
    size_t len = 0u;
    size_t cap = 0u;

    if (dir == NULL) {
        if (bx_archive_fs_handle_error(dir_path,
                                       BX_ARCHIVE_FS_ERROR_OPENDIR,
                                       errno,
                                       error_fn,
                                       error_user_data,
                                       diag) == BX_ARCHIVE_FS_ERROR_SKIP) {
            *names_out = NULL;
            *count_out = 0u;
            return true;
        }
        return false;
    }

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (len == cap) {
            size_t next_cap = cap ? cap * 2u : 16u;
            names = xrealloc(names, next_cap * sizeof(*names));
            cap = next_cap;
        }
        names[len++] = xstrdup(ent->d_name);
    }

    if (closedir(dir) != 0) {
        if (bx_archive_fs_handle_error(dir_path,
                                       BX_ARCHIVE_FS_ERROR_CLOSEDIR,
                                       errno,
                                       error_fn,
                                       error_user_data,
                                       diag) == BX_ARCHIVE_FS_ERROR_SKIP) {
            *names_out = names;
            *count_out = len;
            return true;
        }
        while (len > 0u) {
            free(names[--len]);
        }
        free(names);
        return false;
    }

    if (sort_children && len > 1u) {
        qsort(names, len, sizeof(*names), bx_archive_name_compare);
    }

    *names_out = names;
    *count_out = len;
    return true;
}

static bool bx_archive_fs_visit_path_filtered_inner(struct bx_archive_fs_path_buf* source_path,
                                                    struct bx_archive_fs_path_buf* archive_path,
                                                    bool recurse,
                                                    bool sort_children,
                                                    bx_archive_fs_include_fn include_fn,
                                                    void* include_user_data,
                                                    bx_archive_fs_error_fn error_fn,
                                                    void* error_user_data,
                                                    bx_archive_fs_visit_fn visit_fn,
                                                    void* visit_user_data,
                                                    struct bx_diag_ctx* diag);

static bool bx_archive_fs_visit_sorted_children(struct bx_archive_fs_path_buf* source_path,
                                                struct bx_archive_fs_path_buf* archive_path,
                                                bool sort_children,
                                                bx_archive_fs_include_fn include_fn,
                                                void* include_user_data,
                                                bx_archive_fs_error_fn error_fn,
                                                void* error_user_data,
                                                bx_archive_fs_visit_fn visit_fn,
                                                void* visit_user_data,
                                                struct bx_diag_ctx* diag) {
    char** children = NULL;
    size_t child_count = 0u;
    size_t i;

    if (!bx_archive_read_children(source_path->data,
                                  sort_children,
                                  &children,
                                  &child_count,
                                  error_fn,
                                  error_user_data,
                                  diag)) {
        return false;
    }

    for (i = 0u; i < child_count; i++) {
        size_t source_restore_len = bx_archive_fs_path_buf_push_child(source_path, children[i]);
        size_t archive_restore_len = bx_archive_fs_path_buf_push_child(archive_path, children[i]);
        bool ok = bx_archive_fs_visit_path_filtered_inner(source_path,
                                                          archive_path,
                                                          true,
                                                          sort_children,
                                                          include_fn,
                                                          include_user_data,
                                                          error_fn,
                                                          error_user_data,
                                                          visit_fn,
                                                          visit_user_data,
                                                          diag);

        bx_archive_fs_path_buf_restore(archive_path, archive_restore_len);
        bx_archive_fs_path_buf_restore(source_path, source_restore_len);
        free(children[i]);
        if (!ok) {
            while (++i < child_count) {
                free(children[i]);
            }
            free(children);
            return false;
        }
    }

    free(children);
    return true;
}

static bool bx_archive_fs_visit_unsorted_children(struct bx_archive_fs_path_buf* source_path,
                                                  struct bx_archive_fs_path_buf* archive_path,
                                                  bool sort_children,
                                                  bx_archive_fs_include_fn include_fn,
                                                  void* include_user_data,
                                                  bx_archive_fs_error_fn error_fn,
                                                  void* error_user_data,
                                                  bx_archive_fs_visit_fn visit_fn,
                                                  void* visit_user_data,
                                                  struct bx_diag_ctx* diag) {
    DIR* dir = opendir(source_path->data);
    struct dirent* ent;

    if (dir == NULL) {
        return bx_archive_fs_handle_error(source_path->data,
                                          BX_ARCHIVE_FS_ERROR_OPENDIR,
                                          errno,
                                          error_fn,
                                          error_user_data,
                                          diag) == BX_ARCHIVE_FS_ERROR_SKIP;
    }

    while ((ent = readdir(dir)) != NULL) {
        size_t source_restore_len;
        size_t archive_restore_len;
        bool ok;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        source_restore_len = bx_archive_fs_path_buf_push_child(source_path, ent->d_name);
        archive_restore_len = bx_archive_fs_path_buf_push_child(archive_path, ent->d_name);
        ok = bx_archive_fs_visit_path_filtered_inner(source_path,
                                                     archive_path,
                                                     true,
                                                     sort_children,
                                                     include_fn,
                                                     include_user_data,
                                                     error_fn,
                                                     error_user_data,
                                                     visit_fn,
                                                     visit_user_data,
                                                     diag);
        bx_archive_fs_path_buf_restore(archive_path, archive_restore_len);
        bx_archive_fs_path_buf_restore(source_path, source_restore_len);
        if (!ok) {
            closedir(dir);
            return false;
        }
    }

    if (closedir(dir) != 0) {
        return bx_archive_fs_handle_error(source_path->data,
                                          BX_ARCHIVE_FS_ERROR_CLOSEDIR,
                                          errno,
                                          error_fn,
                                          error_user_data,
                                          diag) == BX_ARCHIVE_FS_ERROR_SKIP;
    }

    return true;
}

static bool bx_archive_fs_visit_path_filtered_inner(struct bx_archive_fs_path_buf* source_path,
                                                    struct bx_archive_fs_path_buf* archive_path,
                                                    bool recurse,
                                                    bool sort_children,
                                                    bx_archive_fs_include_fn include_fn,
                                                    void* include_user_data,
                                                    bx_archive_fs_error_fn error_fn,
                                                    void* error_user_data,
                                                    bx_archive_fs_visit_fn visit_fn,
                                                    void* visit_user_data,
                                                    struct bx_diag_ctx* diag) {
    struct stat st;
    char* link_target = NULL;

    if (lstat(source_path->data, &st) != 0) {
        return bx_archive_fs_handle_error(source_path->data,
                                          BX_ARCHIVE_FS_ERROR_LSTAT,
                                          errno,
                                          error_fn,
                                          error_user_data,
                                          diag) == BX_ARCHIVE_FS_ERROR_SKIP;
    }

    if (include_fn != NULL
        && !include_fn(source_path->data, archive_path->data, &st, include_user_data)) {
        return true;
    }

    if (S_ISLNK(st.st_mode)) {
        link_target = bx_path_readlink_dup(source_path->data);
        if (link_target == NULL) {
            return bx_archive_fs_handle_error(source_path->data,
                                              BX_ARCHIVE_FS_ERROR_READLINK,
                                              errno,
                                              error_fn,
                                              error_user_data,
                                              diag) == BX_ARCHIVE_FS_ERROR_SKIP;
        }
    }

    if (!visit_fn(&(struct bx_archive_fs_visit_entry){
                      .source_path = source_path->data,
                      .archive_path = archive_path->data,
                      .st = &st,
                      .link_target = link_target,
                  },
                  visit_user_data,
                  diag)) {
        free(link_target);
        return false;
    }
    free(link_target);

    if (!recurse || !S_ISDIR(st.st_mode)) {
        return true;
    }
    if (sort_children) {
        return bx_archive_fs_visit_sorted_children(source_path,
                                                   archive_path,
                                                   sort_children,
                                                   include_fn,
                                                   include_user_data,
                                                   error_fn,
                                                   error_user_data,
                                                   visit_fn,
                                                   visit_user_data,
                                                   diag);
    }
    return bx_archive_fs_visit_unsorted_children(source_path,
                                                 archive_path,
                                                 sort_children,
                                                 include_fn,
                                                 include_user_data,
                                                 error_fn,
                                                 error_user_data,
                                                 visit_fn,
                                                 visit_user_data,
                                                 diag);
}

bool bx_archive_fs_visit_path_filtered(const char* source_path,
                                       const char* archive_path,
                                       bool recurse,
                                       bool sort_children,
                                       bx_archive_fs_include_fn include_fn,
                                       void* include_user_data,
                                       bx_archive_fs_error_fn error_fn,
                                       void* error_user_data,
                                       bx_archive_fs_visit_fn visit_fn,
                                       void* visit_user_data,
                                       struct bx_diag_ctx* diag) {
    struct bx_archive_fs_path_buf source_buf = {0};
    struct bx_archive_fs_path_buf archive_buf = {0};
    bool ok;

    bx_archive_fs_path_buf_init(&source_buf, source_path);
    bx_archive_fs_path_buf_init(&archive_buf, archive_path);
    ok = bx_archive_fs_visit_path_filtered_inner(&source_buf,
                                                 &archive_buf,
                                                 recurse,
                                                 sort_children,
                                                 include_fn,
                                                 include_user_data,
                                                 error_fn,
                                                 error_user_data,
                                                 visit_fn,
                                                 visit_user_data,
                                                 diag);
    bx_archive_fs_path_buf_cleanup(&archive_buf);
    bx_archive_fs_path_buf_cleanup(&source_buf);
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
    return bx_archive_fs_visit_path_filtered(source_path,
                                             archive_path,
                                             recurse,
                                             sort_children,
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
}

void bx_archive_parent_dir_cache_invalidate(struct bx_archive_parent_dir_cache* cache) {
    bx_archive_parent_dir_cache_cleanup(cache);
}

bool bx_archive_parent_dir_cache_matches_parent(const struct bx_archive_parent_dir_cache* cache,
                                                const char* parent) {
    return cache != NULL
        && cache->last_parent != NULL
        && parent != NULL
        && strcmp(cache->last_parent, parent) == 0;
}

void bx_archive_parent_dir_cache_remember_parent(struct bx_archive_parent_dir_cache* cache,
                                                 const char* parent) {
    if (cache == NULL) {
        return;
    }

    free(cache->last_parent);
    cache->last_parent = xstrdup(parent);
}

bool bx_archive_ensure_parent_dirs_cached(const char* path,
                                          struct bx_archive_parent_dir_cache* cache,
                                          struct bx_diag_ctx* diag) {
    char* parent = bx_path_parent_dir_dup(path);
    char* cursor;
    size_t i;

    if (parent == NULL) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    if (strcmp(parent, ".") == 0 || strcmp(parent, "/") == 0) {
        free(parent);
        return true;
    }
    if (bx_archive_parent_dir_cache_matches_parent(cache, parent)) {
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
        if (cursor[i] != '/') {
            continue;
        }
        cursor[i] = '\0';
        if (cursor[0] != '\0' && mkdir(cursor, 0777u) != 0 && errno != EEXIST) {
            bx_diag(diag, "%s: %s", cursor, strerror(errno));
            free(parent);
            free(cursor);
            return false;
        }
        cursor[i] = '/';
    }

    if (mkdir(cursor, 0777u) != 0 && errno != EEXIST) {
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

bool bx_archive_ensure_parent_dirs(const char* path, struct bx_diag_ctx* diag) {
    return bx_archive_ensure_parent_dirs_cached(path, NULL, diag);
}

void bx_archive_pending_dirs_free(struct bx_archive_pending_dirs* dirs) {
    size_t i;
    for (i = 0u; i < dirs->len; i++) {
        free(dirs->entries[i].path);
    }
    free(dirs->entries);
    dirs->entries = NULL;
    dirs->len = 0u;
    dirs->cap = 0u;
}

bool bx_archive_pending_dirs_record(struct bx_archive_pending_dirs* dirs,
                                    const char* path,
                                    mode_t mode,
                                    bool set_mtime,
                                    struct timespec mtime) {
    struct bx_archive_pending_dir* entry;
    if (dirs->len == dirs->cap) {
        size_t next_cap = dirs->cap ? dirs->cap * 2u : 16u;
        dirs->entries = xrealloc(dirs->entries, next_cap * sizeof(*dirs->entries));
        dirs->cap = next_cap;
    }
    entry = &dirs->entries[dirs->len++];
    entry->path = xstrdup(path);
    entry->mode = mode;
    entry->mtime = mtime;
    entry->set_mtime = set_mtime;
    return true;
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

bool bx_archive_pending_dirs_apply(struct bx_archive_pending_dirs* dirs,
                                   struct bx_diag_ctx* diag) {
    while (dirs->len > 0u) {
        struct bx_archive_pending_dir* entry = &dirs->entries[dirs->len - 1u];
        if (chmod(entry->path, entry->mode & 07777u) != 0) {
            bx_diag(diag, "%s: %s", entry->path, strerror(errno));
            return false;
        }
        if (entry->set_mtime && !bx_archive_set_path_mtime(entry->path, entry->mtime, false, diag)) {
            return false;
        }
        free(entry->path);
        dirs->len--;
    }
    return true;
}
