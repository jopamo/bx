#define _GNU_SOURCE
#include "lib/dir_path.h"
#include "lib/fd_ops.h"
#include "lib/path_ops.h"
#include "bx/libbx.h"

#include <stdlib.h>
#include <string.h>

/* Consumes parent and path on both success and failure. Public entry points
 * enforce their different source/extraction policies before this walk. */
static int bx_dir_path_walk_parent(int parent, char* path, int flags,
                                    bool create, mode_t mode, char** leaf) {
    char* component = path;
    char* separator;
    while ((separator = strchr(component, '/')) != NULL) {
        *separator = '\0';
        if (!*component || strcmp(component, ".") == 0) {
            component = separator + 1;
            continue;
        }
        int child = bx_fd_openat_cloexec(parent, component, flags | O_DIRECTORY | O_NOFOLLOW, 0);
        if (child < 0 && errno == ENOENT && create) {
            if (bx_fd_mkdirat_child(parent, component, mode) != 0 && errno != EEXIST)
                goto fail;
            child = bx_fd_openat_cloexec(parent, component, flags | O_DIRECTORY | O_NOFOLLOW, 0);
        }
        if (child < 0)
            goto fail;
        close(parent);
        parent = child;
        component = separator + 1;
    }
    *leaf = xstrdup(*component ? component : ".");
    free(path);
    return parent;

fail: {
    int error = errno;
    close(parent);
    free(path);
    errno = error;
    return -1;
}
}

int bx_dir_path_open_parent(int root_fd, const char* path, bool create, mode_t mode, char** leaf) {
    if (root_fd < 0 || path == NULL || !*path || leaf == NULL || bx_path_is_absolute(path) || bx_path_has_parent_reference(path)) {
        errno = EINVAL;
        return -1;
    }
    struct stat status;
    if (fstat(root_fd, &status) != 0)
        return -1;
    if (!S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    int parent = bx_fd_dup_cloexec(root_fd);
    if (parent < 0)
        return -1;
    return bx_dir_path_walk_parent(parent, bx_path_normalize_relative_lexical_dup(path),
                                    O_RDONLY, create, mode, leaf);
}

int bx_dir_path_open_source_parent(const char* path, char** leaf) {
    if (!path || !leaf) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(path);
    if (!len) {
        errno = ENOENT;
        return -1;
    }
    int root = bx_fd_open_cloexec(path[0] == '/' ? "/" : ".", O_PATH | O_DIRECTORY, 0);
    if (root < 0)
        return -1;
    char* name = NULL;
    char* components = bx_path_strip_trailing_slashes_dup(path);
    int parent = bx_dir_path_walk_parent(root, components, O_PATH, false, 0, &name);
    if (parent < 0)
        return -1;
    if (path[len - 1] == '/') {
        struct stat status;
        int error = fstatat(parent, name, &status, AT_SYMLINK_NOFOLLOW) != 0 ? errno
            : !S_ISDIR(status.st_mode) ? ENOTDIR : 0;
        if (error) {
            close(parent);
            free(name);
            errno = error;
            return -1;
        }
    }
    *leaf = name;
    return parent;
}
