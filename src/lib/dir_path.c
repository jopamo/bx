#include "lib/dir_path.h"
#include "lib/fd_ops.h"
#include "lib/path_ops.h"
#include "bx/libbx.h"

#include <stdlib.h>
#include <string.h>

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
    char* normalized = bx_path_normalize_relative_lexical_dup(path);
    int parent = bx_fd_dup_cloexec(root_fd);
    if (parent < 0) {
        free(normalized);
        return -1;
    }
    char* component = normalized;
    char* separator;
    while ((separator = strchr(component, '/')) != NULL) {
        *separator = '\0';
        int child = bx_fd_openat_child_nofollow(parent, component, O_RDONLY | O_DIRECTORY, 0);
        if (child < 0 && errno == ENOENT && create) {
            if (bx_fd_mkdirat_child(parent, component, mode) != 0 && errno != EEXIST)
                goto fail;
            child = bx_fd_openat_child_nofollow(parent, component, O_RDONLY | O_DIRECTORY, 0);
        }
        if (child < 0)
            goto fail;
        close(parent);
        parent = child;
        component = separator + 1;
    }
    *leaf = xstrdup(component);
    free(normalized);
    return parent;

fail: {
    int error = errno;
    close(parent);
    free(normalized);
    errno = error;
    return -1;
}
}
