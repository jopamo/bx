#define _GNU_SOURCE
#include "lib/dir_path.h"
#include "lib/fd_ops.h"
#include "lib/path_ops.h"
#include "lib/random_bytes.h"
#include "bx/libbx.h"

#include <stdlib.h>
#include <string.h>

/* Consumes parent and path on both success and failure. Public entry points
 * enforce their different source/extraction policies before this walk. */
static int bx_dir_path_walk_parent(int parent, char* path, int flags,
                                    bool create, bool replace, mode_t mode, char** leaf) {
    char* component = path;
    char* separator;
    while ((separator = strchr(component, '/')) != NULL) {
        *separator = '\0';
        if (!*component || strcmp(component, ".") == 0) {
            component = separator + 1;
            continue;
        }
        int child = bx_fd_openat_cloexec(parent, component, flags | O_DIRECTORY | O_NOFOLLOW, 0);
        if (child < 0 && create && replace && (errno == ENOTDIR || errno == ELOOP)) {
            struct stat status;
            if (fstatat(parent, component, &status, AT_SYMLINK_NOFOLLOW) != 0)
                goto fail;
            if (!S_ISDIR(status.st_mode)) {
                if (bx_fd_unlinkat_child(parent, component, 0) != 0)
                    goto fail;
                errno = ENOENT;
            }
        }
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
    return bx_dir_path_open_destination_parent(root_fd, path, BX_DIR_PATH_CONFINED,
                                                create, mode, leaf);
}

int bx_dir_path_open_destination_parent(int root_fd, const char* path,
                                        unsigned policy, bool create,
                                        mode_t mode, char** leaf) {
    if (root_fd < 0 || path == NULL || !*path || leaf == NULL
        || (!(policy & BX_DIR_PATH_ALLOW_EXTERNAL)
            && (bx_path_is_absolute(path) || bx_path_has_parent_reference(path)))) {
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
    int parent = path[0] == '/' ? bx_fd_open_cloexec("/", O_PATH | O_DIRECTORY, 0)
        : bx_fd_dup_cloexec(root_fd);
    if (parent < 0)
        return -1;
    return bx_dir_path_walk_parent(parent, bx_path_strip_trailing_slashes_dup(path),
                                    O_PATH, create, policy & BX_DIR_PATH_REPLACE_NON_DIRS,
                                    mode, leaf);
}

ptrdiff_t bx_dir_path_depth(const char* path, ptrdiff_t base) {
    if (*path == '/')
        base = 0;
    for (const char* part = path; *part;) {
        if (*part == '/') {
            part++;
            continue;
        }
        size_t len = strcspn(part, "/");
        if (len == 2u && strncmp(part, "..", len) == 0) {
            if (base > 0)
                base--;
        }
        else if (len != 1u || *part != '.')
            base++;
        part += len;
    }
    return base;
}

bool bx_dir_path_mkdtemp_at(int parent_fd, char* name) {
    if (!bx_fd_at_name_is_child(name) || strlen(name) < 6u
        || strcmp(name + strlen(name) - 6u, "XXXXXX") != 0) {
        errno = EINVAL;
        return false;
    }
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    char* suffix = name + strlen(name) - 6u;
    for (unsigned attempt = 0; attempt < 128u; attempt++) {
        unsigned char bytes[6];
        if (!bx_random_bytes(bytes, sizeof(bytes)))
            return false;
        for (size_t i = 0; i < sizeof(bytes); i++)
            suffix[i] = alphabet[bytes[i] % (sizeof(alphabet) - 1u)];
        if (bx_fd_mkdirat_child(parent_fd, name, 0700) == 0)
            return true;
        if (errno != EEXIST)
            return false;
    }
    errno = EEXIST;
    return false;
}

int bx_dir_path_open_source_parent_at(int start_fd, const char* path, char** leaf) {
    if (!path || !leaf) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(path);
    if (!len) {
        errno = ENOENT;
        return -1;
    }
    int root = path[0] == '/' ? bx_fd_open_cloexec("/", O_PATH | O_DIRECTORY, 0)
        : bx_fd_openat_cloexec(start_fd, ".", O_PATH | O_DIRECTORY, 0);
    if (root < 0)
        return -1;
    char* name = NULL;
    char* components = bx_path_strip_trailing_slashes_dup(path);
    int parent = bx_dir_path_walk_parent(root, components, O_PATH, false, false, 0, &name);
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

int bx_dir_path_open_source_parent(const char* path, char** leaf) {
    return bx_dir_path_open_source_parent_at(AT_FDCWD, path, leaf);
}
