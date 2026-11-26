#ifndef BX_APPLETS_ARCHIVE_ARCHIVE_FS_H
#define BX_APPLETS_ARCHIVE_ARCHIVE_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

#include "bx/diag.h"
#include "applets/archive/archive_common.h"
#include "lib/file_metadata.h"
#include "lib/mount_identity.h"

struct bx_archive_fs_entry {
    char* source_path;
    char* archive_path;
    struct stat st;
    char* link_target;
    /* Buffered readers verify the parent before opening the leaf. */
    dev_t source_parent_dev;
    ino_t source_parent_ino;
    /* Nonzero for -h: boundary between the operand and readdir components. */
    size_t follow_operand_len;
    size_t chdir_count, source_offset;
    struct bx_mount_identity source_mount; /* has_mount when --one-file-system selected this entry */
};

struct bx_archive_fs_list {
    /* Ordered -h create directory changes, shared by all buffered entries. */
    struct bx_archive_name_list chdirs;
    struct bx_archive_fs_entry* entries;
    size_t len;
    size_t cap;
};

struct bx_archive_fs_visit_entry {
    /* Borrowed source authority, valid only during the visitor call. */
    int source_parent_fd;
    /* Pinned symlink FD, or -1. Owned by the walker, including on failure. */
    int source_fd;
    const char* source_name; /* Empty for an -h directory pinned by source_parent_fd itself. */
    const char* source_path;
    const char* archive_path;
    const struct stat* st;
    const char* link_target;
    size_t follow_operand_len;
    struct bx_mount_identity source_mount;
    bool mount_boundary; /* Include filters may inspect the directory, not its children. */
};

enum bx_archive_fs_error_op {
    BX_ARCHIVE_FS_ERROR_LSTAT = 0,
    BX_ARCHIVE_FS_ERROR_READLINK,
    BX_ARCHIVE_FS_ERROR_OPENDIR,
    BX_ARCHIVE_FS_ERROR_READDIR,
    BX_ARCHIVE_FS_ERROR_CLOSEDIR,
};

enum bx_archive_fs_error_action {
    BX_ARCHIVE_FS_ERROR_ABORT = 0,
    BX_ARCHIVE_FS_ERROR_SKIP,
};

/* With dereference, name-only exclusions run before source lookup. */
typedef bool (*bx_archive_fs_include_name_fn)(const char* source_path, const char* archive_path, void* user_data);
/* Runs on the selected object; borrows the same parent/name as the visitor. */
typedef bool (*bx_archive_fs_include_fn)(const struct bx_archive_fs_visit_entry* entry,
                                         void* user_data);
typedef enum bx_archive_fs_error_action (*bx_archive_fs_error_fn)(const char* source_path,
                                                                  enum bx_archive_fs_error_op op,
                                                                  int errnum,
                                                                  void* user_data);
typedef bool (*bx_archive_fs_visit_fn)(const struct bx_archive_fs_visit_entry* entry,
                                       void* user_data,
                                       struct bx_diag_ctx* diag);

struct bx_archive_pending_metadata_entry {
    struct bx_file_restore restore;
    char* path;
    size_t boundary_prefix;
    dev_t dev;
    ino_t ino;
    mode_t type;
    ptrdiff_t depth;
    uint64_t order;
    uint64_t origin;
    bool alias;
};

struct bx_archive_pending_metadata {
    struct bx_archive_pending_metadata_entry* entries;
    size_t len;
    size_t cap;
    size_t bytes;
    unsigned path_policy;
    /* Absolute anchor depth when absolute and relative locators coexist. */
    ptrdiff_t root_depth;
};

#define BX_ARCHIVE_PENDING_METADATA_LIMIT 1048576u

void bx_archive_fs_list_free(struct bx_archive_fs_list* list);
/* Reopen a buffered source using the same policy as the original walk. */
int bx_archive_fs_reopen_source(int root_fd, const struct bx_archive_fs_entry* entry, char** leaf);
bool bx_archive_fs_follow_chdir(int* cwd_fd, const char* path, struct bx_diag_ctx* diag);
/* Return an owned descriptor for a named or already pinned source directory. */
int bx_archive_fs_open_source_dir(int parent, const char* name, bool readable);
/* Resolve the operand relative to the borrowed root; source_path is for display. */
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
                                       struct bx_diag_ctx* diag);
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
                                     struct bx_diag_ctx* diag);
bool bx_archive_fs_add_path(struct bx_archive_fs_list* list,
                            const char* source_path,
                            const char* archive_path,
                            bool recurse,
                            bool sort_children,
                            struct bx_diag_ctx* diag);

void bx_archive_pending_metadata_free(struct bx_archive_pending_metadata* dirs);
/* Borrows a regular-file/directory fd and copies metadata without retaining it.
 * Paths are approved destinations relative to root_fd unless explicitly absolute.
 * Bound records to 1048576 entries and 256 MiB of owned snapshot data.
 * Order snapshots by producer sequence; origin marks regular-inode creation. */
bool bx_archive_pending_metadata_record_fd(struct bx_archive_pending_metadata* dirs,
                                           int fd,
                                           const char* path,
                                           size_t boundary_prefix,
                                           const struct bx_file_restore* restore,
                                           uint64_t order,
                                           uint64_t origin);
/* Register another locator for a deferred regular inode, without replaying
 * the alias header's metadata. Shares the snapshot count/byte bounds. */
bool bx_archive_pending_metadata_record_alias_fd(struct bx_archive_pending_metadata* dirs,
                                                 int fd, const char* path,
                                                 size_t boundary_prefix,
                                                 uint64_t origin);
/* Reopen a saved live location transiently. Return -2 for a stale/missing
 * locator, -1 for an operation error. Writable targets must be regular. */
int bx_archive_pending_metadata_open_fd(int root_fd, unsigned policy, const struct bx_archive_pending_metadata_entry* expected, bool writable);
bool bx_archive_pending_metadata_apply(struct bx_archive_pending_metadata* dirs, int root_fd, struct bx_diag_ctx* diag);
bool bx_archive_restore_fd(const struct bx_file_restore* restore, int fd,
                            const char* path,
                            struct bx_diag_ctx* diag);
bool bx_archive_prepare_regular_fd(const struct bx_file_restore* restore, int fd, const char* path, struct bx_diag_ctx* diag);
bool bx_archive_restore_leaf(const struct bx_file_restore* restore, int parent_fd, const char* leaf, const struct stat* expected, const char* path, struct bx_diag_ctx* diag);

#endif /* BX_APPLETS_ARCHIVE_ARCHIVE_FS_H */
