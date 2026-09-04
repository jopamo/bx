#ifndef MIRA_SECURE_PATH_H
#define MIRA_SECURE_PATH_H

/* MIRA_HEADER_OWNER: util */
/* MIRA_HEADER_CONSUMERS: util, fs, store */

/*
 * Linux path-resolution policy shared by filesystem and persistent-store
 * code. Every open rejects symlinks in all path components; there is no
 * weaker fallback.
 */

#include <stdbool.h>
#include <sys/types.h>

int mira_secure_path_check_resolution(void);
/*
 * Opens one leaf relative to dirfd, rejecting symlinks and mount crossings.
 * `path` must be a simple name, not a multi-component path.
 */
int mira_secure_path_open_leaf(int dirfd, const char* path, int flags, mode_t mode);
int mira_secure_path_rename_leaf_noreplace(int dirfd, const char* old_name, const char* new_name);
int mira_secure_path_open_existing_file(const char* path);
int mira_secure_path_open_parent_directory(const char* path, bool create_missing, char** basename_out);
int mira_secure_path_unlink_file(const char* path);

/* Splits path into an optional parent and a required non-empty leaf. */
int mira_secure_path_split(const char* path, char** parent_out, char** basename_out);

#endif
