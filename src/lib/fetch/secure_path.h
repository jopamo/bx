#ifndef BX_FETCH_SECURE_PATH_H
#define BX_FETCH_SECURE_PATH_H

/* BX_FETCH_HEADER_OWNER: util */
/* BX_FETCH_HEADER_CONSUMERS: util, fs, store */

/*
 * Linux path-resolution policy shared by filesystem and persistent-store
 * code. Every open rejects symlinks in all path components; there is no
 * weaker fallback. Generic descriptor operations and child-name validation
 * use bx fd_ops; fetch retains openat2's component and mount restrictions.
 */

#include <stdbool.h>
#include <sys/types.h>

int bx_fetch_secure_path_check_resolution(void);
/*
 * Opens one leaf relative to dirfd, rejecting symlinks and mount crossings.
 * Leaf operations require a retained directory descriptor, never AT_FDCWD.
 * Names must be simple children, not multi-component paths or "."/"..".
 */
int bx_fetch_secure_path_open_leaf(int dirfd, const char* path, int flags, mode_t mode);
int bx_fetch_secure_path_rename_leaf_noreplace(int dirfd, const char* old_name, const char* new_name);
int bx_fetch_secure_path_exchange_leaves(int dirfd, const char* first_name, const char* second_name);
int bx_fetch_secure_path_open_existing_file(const char* path);
int bx_fetch_secure_path_open_existing_directory(const char* path);
int bx_fetch_secure_path_open_parent_directory(const char* path, bool create_missing, char** basename_out);
int bx_fetch_secure_path_unlink_file(const char* path);

/*
 * Allocates an optional parent and a required non-empty leaf using path_ops
 * lexical splitting with trailing slashes stripped. Earlier separators are
 * preserved; this does not validate child-name or filesystem authority.
 * parent_out may be NULL only for paths without a parent. Allocation failure
 * is recoverable and leaves both supplied outputs NULL.
 */
int bx_fetch_secure_path_split(const char* path, char** parent_out, char** basename_out);

#endif
