#ifndef BX_APPLETS_ARCHIVE_ARCHIVE_FS_H
#define BX_APPLETS_ARCHIVE_ARCHIVE_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <time.h>

#include "bx/diag.h"

struct bx_archive_fs_entry {
    char* source_path;
    char* archive_path;
    struct stat st;
    char* link_target;
};

struct bx_archive_fs_list {
    struct bx_archive_fs_entry* entries;
    size_t len;
    size_t cap;
};

struct bx_archive_fs_visit_entry {
    const char* source_path;
    const char* archive_path;
    const struct stat* st;
    const char* link_target;
};

enum bx_archive_fs_error_op {
    BX_ARCHIVE_FS_ERROR_LSTAT = 0,
    BX_ARCHIVE_FS_ERROR_READLINK,
    BX_ARCHIVE_FS_ERROR_OPENDIR,
    BX_ARCHIVE_FS_ERROR_CLOSEDIR,
};

enum bx_archive_fs_error_action {
    BX_ARCHIVE_FS_ERROR_ABORT = 0,
    BX_ARCHIVE_FS_ERROR_SKIP,
};

typedef bool (*bx_archive_fs_include_fn)(const char* source_path,
                                         const char* archive_path,
                                         const struct stat* st,
                                         void* user_data);
typedef enum bx_archive_fs_error_action (*bx_archive_fs_error_fn)(const char* source_path,
                                                                  enum bx_archive_fs_error_op op,
                                                                  int errnum,
                                                                  void* user_data);
typedef bool (*bx_archive_fs_visit_fn)(const struct bx_archive_fs_visit_entry* entry,
                                       void* user_data,
                                       struct bx_diag_ctx* diag);

struct bx_archive_pending_dir {
    char* path;
    mode_t mode;
    struct timespec mtime;
    bool set_mtime;
};

struct bx_archive_pending_dirs {
    struct bx_archive_pending_dir* entries;
    size_t len;
    size_t cap;
};

struct bx_archive_parent_dir_cache {
    char* last_parent;
    size_t last_parent_len;
};

void bx_archive_fs_list_free(struct bx_archive_fs_list* list);
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
                                       struct bx_diag_ctx* diag);
bool bx_archive_fs_add_path_filtered(struct bx_archive_fs_list* list,
                                     const char* source_path,
                                     const char* archive_path,
                                     bool recurse,
                                     bool sort_children,
                                     bx_archive_fs_include_fn include_fn,
                                     void* include_user_data,
                                     bx_archive_fs_error_fn error_fn,
                                     void* error_user_data,
                                     struct bx_diag_ctx* diag);
bool bx_archive_fs_add_path(struct bx_archive_fs_list* list,
                            const char* source_path,
                            const char* archive_path,
                            bool recurse,
                            bool sort_children,
                            struct bx_diag_ctx* diag);

void bx_archive_parent_dir_cache_cleanup(struct bx_archive_parent_dir_cache* cache);
void bx_archive_parent_dir_cache_invalidate(struct bx_archive_parent_dir_cache* cache);
bool bx_archive_parent_dir_cache_matches_parent(const struct bx_archive_parent_dir_cache* cache,
                                                const char* parent);
void bx_archive_parent_dir_cache_remember_parent(struct bx_archive_parent_dir_cache* cache,
                                                 const char* parent);

bool bx_archive_ensure_parent_dirs(const char* path, struct bx_diag_ctx* diag);
bool bx_archive_ensure_parent_dirs_cached(const char* path,
                                          struct bx_archive_parent_dir_cache* cache,
                                          struct bx_diag_ctx* diag);
bool bx_archive_ensure_parent_dirs_safe(const char* path, struct bx_diag_ctx* diag);
bool bx_archive_ensure_parent_dirs_safe_cached(const char* path,
                                               struct bx_archive_parent_dir_cache* cache,
                                               struct bx_diag_ctx* diag);
bool bx_archive_remove_path_tree(const char* path, struct bx_diag_ctx* diag);

void bx_archive_pending_dirs_free(struct bx_archive_pending_dirs* dirs);
bool bx_archive_pending_dirs_record(struct bx_archive_pending_dirs* dirs,
                                    const char* path,
                                    mode_t mode,
                                    bool set_mtime,
                                    struct timespec mtime);
bool bx_archive_pending_dirs_apply(struct bx_archive_pending_dirs* dirs,
                                   struct bx_diag_ctx* diag);

bool bx_archive_set_path_mtime(const char* path,
                               struct timespec mtime,
                               bool nofollow,
                               struct bx_diag_ctx* diag);
bool bx_archive_set_fd_mtime(int fd,
                             const char* path,
                             struct timespec mtime,
                             struct bx_diag_ctx* diag);

#endif /* BX_APPLETS_ARCHIVE_ARCHIVE_FS_H */
