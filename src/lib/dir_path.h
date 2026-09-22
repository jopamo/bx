#ifndef BX_LIB_DIR_PATH_H
#define BX_LIB_DIR_PATH_H

#include <stdbool.h>
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

#endif
