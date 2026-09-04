#define _GNU_SOURCE
#include "lib/fetch/secure_path.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/openat2.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int secure_path_fail_errno(int error_number) {
    errno = error_number;
    return -1;
}

static int secure_path_fail_parent(int error_number, int dirfd, char* walk, char* parent, char* basename) {
    free(walk);
    close(dirfd);
    free(parent);
    free(basename);
    return secure_path_fail_errno(error_number);
}

static int secure_path_openat(int dirfd, const char* path, int flags, mode_t mode, unsigned long long resolve) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    struct open_how how = {
        .flags = (unsigned long long)flags,
        .mode = (unsigned long long)mode,
        .resolve = resolve,
    };
    return (int)syscall(SYS_openat2, dirfd, path, &how, sizeof(how));
}

static bool secure_path_is_simple_leaf(const char* path) {
    return path && path[0] != '\0' && strcmp(path, ".") != 0 && strcmp(path, "..") != 0 && strchr(path, '/') == NULL;
}

int mira_secure_path_open_leaf(int dirfd, const char* path, int flags, mode_t mode) {
    if (!secure_path_is_simple_leaf(path)) {
        errno = EINVAL;
        return -1;
    }
    return secure_path_openat(dirfd, path, flags, mode, RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV);
}

int mira_secure_path_rename_leaf_noreplace(int dirfd, const char* old_name, const char* new_name) {
    if (dirfd == -1 || !secure_path_is_simple_leaf(old_name) || !secure_path_is_simple_leaf(new_name)) {
        errno = EINVAL;
        return -1;
    }
    return (int)syscall(SYS_renameat2, dirfd, old_name, dirfd, new_name, RENAME_NOREPLACE);
}

int mira_secure_path_check_resolution(void) {
    int fd = secure_path_openat(AT_FDCWD, "/", O_PATH | O_DIRECTORY | O_CLOEXEC, 0, RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV);
    if (fd == -1)
        return -1;
    if (close(fd) != 0)
        return -1;

    /*
     * Probe atomic no-replace publication without naming a mutable object.
     * An empty source is guaranteed to fail with ENOENT after the kernel has
     * accepted the syscall and flag.
     */
    if (syscall(SYS_renameat2, AT_FDCWD, "", AT_FDCWD, "", RENAME_NOREPLACE) == -1 && errno == ENOENT) {
        return 0;
    }
    if (errno == 0)
        errno = EIO;
    return -1;
}

int mira_secure_path_open_existing_file(const char* path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    char* basename = NULL;
    int parent_fd = mira_secure_path_open_parent_directory(path, false, &basename);
    if (parent_fd == -1)
        return -1;

    int fd = mira_secure_path_open_leaf(parent_fd, basename, O_RDONLY | O_CLOEXEC, 0);
    int open_error_number = errno;
    close(parent_fd);
    free(basename);
    if (fd == -1) {
        errno = open_error_number;
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int error_number = errno;
        close(fd);
        errno = error_number;
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

int mira_secure_path_split(const char* path, char** parent_out, char** basename_out) {
    if (!path || !basename_out) {
        errno = EINVAL;
        return -1;
    }

    *basename_out = NULL;
    if (parent_out)
        *parent_out = NULL;

    char* copy = strdup(path);
    if (!copy)
        return -1;

    size_t len = strlen(copy);
    while (len > 0 && copy[len - 1] == '/') {
        copy[--len] = '\0';
    }
    if (len == 0) {
        free(copy);
        errno = EINVAL;
        return -1;
    }

    char* slash = strrchr(copy, '/');
    bool has_parent = slash != NULL;
    char* basename = NULL;
    char* parent = NULL;

    if (!slash) {
        basename = strdup(copy);
    }
    else if (slash == copy) {
        basename = strdup(slash + 1);
        parent = strdup("/");
    }
    else {
        basename = strdup(slash + 1);
        *slash = '\0';
        parent = strdup(copy);
    }
    free(copy);

    if (!basename || (has_parent && !parent)) {
        int error_number = errno ? errno : ENOMEM;
        free(parent);
        free(basename);
        errno = error_number;
        return -1;
    }
    if (basename[0] == '\0' || (has_parent && !parent_out)) {
        free(parent);
        free(basename);
        errno = EINVAL;
        return -1;
    }

    if (parent_out)
        *parent_out = parent;
    else
        free(parent);
    *basename_out = basename;
    return 0;
}

int mira_secure_path_open_parent_directory(const char* path, bool create_missing, char** basename_out) {
    if (!path || !basename_out) {
        errno = EINVAL;
        return -1;
    }
    *basename_out = NULL;

    char* parent = NULL;
    char* basename = NULL;
    if (mira_secure_path_split(path, &parent, &basename) != 0) {
        return -1;
    }

    const char* start = path[0] == '/' ? "/" : ".";
    int dirfd = open(start, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd == -1) {
        free(parent);
        free(basename);
        return -1;
    }

    if (parent && !(parent[0] == '/' && parent[1] == '\0')) {
        char* walk = strdup(parent);
        if (!walk) {
            close(dirfd);
            free(parent);
            free(basename);
            return -1;
        }

        char* cursor = walk;
        if (path[0] == '/' && cursor[0] == '/')
            cursor++;

        char* segment = NULL;
        while ((segment = strsep(&cursor, "/")) != NULL) {
            if (segment[0] == '\0' || strcmp(segment, ".") == 0)
                continue;

            bool created = false;
            if (create_missing) {
                if (mkdirat(dirfd, segment, 0755) == 0) {
                    created = true;
                }
                else if (errno != EEXIST) {
                    return secure_path_fail_parent(errno, dirfd, walk, parent, basename);
                }
            }

            int next_fd = secure_path_openat(dirfd, segment, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0, RESOLVE_NO_SYMLINKS);
            if (next_fd == -1) {
                return secure_path_fail_parent(errno, dirfd, walk, parent, basename);
            }

            if (created && (fsync(next_fd) != 0 || fsync(dirfd) != 0)) {
                int error_number = errno;
                close(next_fd);
                return secure_path_fail_parent(error_number, dirfd, walk, parent, basename);
            }

            close(dirfd);
            dirfd = next_fd;
        }
        free(walk);
    }

    free(parent);
    *basename_out = basename;
    return dirfd;
}

int mira_secure_path_unlink_file(const char* path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    char* basename = NULL;
    int parent_fd = mira_secure_path_open_parent_directory(path, false, &basename);
    if (parent_fd == -1)
        return -1;

    struct stat st;
    int rc = fstatat(parent_fd, basename, &st, AT_SYMLINK_NOFOLLOW);
    int error_number = errno;
    if (rc == 0 && S_ISDIR(st.st_mode)) {
        rc = -1;
        error_number = EISDIR;
    }
    if (rc == 0 && unlinkat(parent_fd, basename, 0) != 0) {
        rc = -1;
        error_number = errno;
    }
    if (close(parent_fd) != 0 && rc == 0) {
        rc = -1;
        error_number = errno;
    }
    free(basename);

    if (rc != 0)
        errno = error_number;
    return rc;
}
