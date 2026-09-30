#ifndef BX_LIB_DIR_PATH_H
#define BX_LIB_DIR_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * Borrow root_fd and resolve a relative path without following symlinks.
 * Absolute paths, empty paths and any ".." component fail before mutation.
 * "." components and repeated separators are normalized. On success return an
 * owned CLOEXEC parent descriptor and an allocated leaf name. A path naming
 * the root itself returns "."; callers must handle that directory explicitly.
 * On failure return -1, preserve errno and leave *leaf unchanged.
 */
int bx_dir_path_open_parent(int root_fd, const char* path, bool create, mode_t mode, char** leaf);

enum bx_dir_path_policy {
    BX_DIR_PATH_CONFINED = 0,
    BX_DIR_PATH_ALLOW_EXTERNAL = 1,
    BX_DIR_PATH_REPLACE_NON_DIRS = 2,
    BX_DIR_PATH_NO_MOUNT_CROSSING = 4,
};

/* Resolve an applet-approved destination from a borrowed anchor. Only the
 * explicit external policy permits absolute paths or "..". Intermediate
 * symlinks are never followed; replacement, if requested, unlinks them.
 * NO_MOUNT_CROSSING checks every component and existing final object against
 * the starting directory ("/" for absolute paths), requiring mount IDs to
 * distinguish bind mounts. Returns an owned parent fd and leaf. Descriptor
 * use is independent of depth. */
int bx_dir_path_open_destination_parent(int root_fd, const char* path,
                                        unsigned policy, bool create,
                                        mode_t mode, char** leaf);
ptrdiff_t bx_dir_path_depth(const char* path, ptrdiff_t base);
/* Create a mode-0700 directory from a single-child template ending in XXXXXX. */
bool bx_dir_path_mkdtemp_at(int parent_fd, char* name);

/* Open the parent of an explicitly selected source operand, without following
 * symlinks in any component. Unlike extraction paths, absolute paths and ".."
 * are allowed; components are resolved in order, never lexically collapsed.
 * A trailing slash requires an existing directory leaf. Returns an owned
 * CLOEXEC parent fd and allocated leaf, or leaves *leaf unchanged on failure. */
int bx_dir_path_open_source_parent(const char* path, char** leaf);
/* Same source policy, resolving relative paths from a borrowed start_fd. */
int bx_dir_path_open_source_parent_at(int start_fd, const char* path, char** leaf);

#endif
