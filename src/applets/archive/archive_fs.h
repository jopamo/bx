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

typedef bool (*bx_archive_fs_include_fn)(const char* source_path,
                                         const char* archive_path,
                                         const struct stat* st,
                                         void* user_data);

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

void bx_archive_fs_list_free(struct bx_archive_fs_list* list);
bool bx_archive_fs_add_path_filtered(struct bx_archive_fs_list* list,
                                     const char* source_path,
                                     const char* archive_path,
                                     bool recurse,
                                     bool sort_children,
                                     bx_archive_fs_include_fn include_fn,
                                     void* include_user_data,
                                     struct bx_diag_ctx* diag);
bool bx_archive_fs_add_path(struct bx_archive_fs_list* list,
                            const char* source_path,
                            const char* archive_path,
                            bool recurse,
                            bool sort_children,
                            struct bx_diag_ctx* diag);

bool bx_archive_ensure_parent_dirs(const char* path, struct bx_diag_ctx* diag);

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

#endif /* BX_APPLETS_ARCHIVE_ARCHIVE_FS_H */
