#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/xattr.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "copy_metadata.h"

static bool bx_xattr_error_missing(int err) {
    return err == ENODATA
#ifdef ENOATTR
           || err == ENOATTR
#endif
        ;
}

static bool bx_xattr_error_unsupported(int err) {
    return err == ENOSYS || err == ENOTSUP
#ifdef EOPNOTSUPP
           || err == EOPNOTSUPP
#endif
        ;
}

static bool bx_xattr_error_ignorable(int err, bool allow_missing) {
    if (allow_missing && bx_xattr_error_missing(err)) {
        return true;
    }
    return bx_xattr_error_unsupported(err);
}

static bool bx_ownership_error_ignorable(int err) {
    /*
     * GNU cp treats ownership preservation as best-effort for
     * unprivileged callers: EPERM/EACCES cover the common "not allowed"
     * cases, and EINVAL covers unmapped IDs in user namespaces.
     */
    return err == EPERM || err == EACCES || err == EINVAL;
}

static bool bx_copy_specific_xattr_fd(int src_fd, int dest_fd, const char* name, bool allow_missing) {
    ssize_t val_size = fgetxattr(src_fd, name, NULL, 0);
    if (val_size < 0) {
        return bx_xattr_error_ignorable(errno, allow_missing);
    }

    char* value = malloc(val_size > 0 ? (size_t)val_size : 1u);
    if (!value)
        return false;

    val_size = fgetxattr(src_fd, name, value, (size_t)val_size);
    if (val_size < 0) {
        int err = errno;
        free(value);
        return bx_xattr_error_ignorable(err, allow_missing);
    }

    if (fsetxattr(dest_fd, name, value, (size_t)val_size, 0) != 0) {
        free(value);
        return false;
    }

    free(value);
    return true;
}

static bool bx_copy_specific_xattr_path(const char* src_path, const char* dest_path, const char* name, bool no_follow, bool allow_missing) {
    ssize_t val_size = no_follow ? lgetxattr(src_path, name, NULL, 0) : getxattr(src_path, name, NULL, 0);
    if (val_size < 0) {
        return bx_xattr_error_ignorable(errno, allow_missing);
    }

    char* value = malloc(val_size > 0 ? (size_t)val_size : 1u);
    if (!value)
        return false;

    val_size = no_follow ? lgetxattr(src_path, name, value, (size_t)val_size) : getxattr(src_path, name, value, (size_t)val_size);
    if (val_size < 0) {
        int err = errno;
        free(value);
        return bx_xattr_error_ignorable(err, allow_missing);
    }

    int rc;
    if (no_follow) {
        rc = lsetxattr(dest_path, name, value, (size_t)val_size, 0);
    }
    else {
        rc = setxattr(dest_path, name, value, (size_t)val_size, 0);
    }
    if (rc != 0) {
        free(value);
        return false;
    }

    free(value);
    return true;
}

static bool bx_copy_acls_fd(int src_fd, int dest_fd) {
    if (!bx_copy_specific_xattr_fd(src_fd, dest_fd, "system.posix_acl_access", true))
        return false;
    if (!bx_copy_specific_xattr_fd(src_fd, dest_fd, "system.posix_acl_default", true))
        return false;
    return true;
}

static bool bx_copy_acls_path(const char* src_path, const char* dest_path, bool no_follow) {
    if (!bx_copy_specific_xattr_path(src_path, dest_path, "system.posix_acl_access", no_follow, true))
        return false;
    if (!bx_copy_specific_xattr_path(src_path, dest_path, "system.posix_acl_default", no_follow, true))
        return false;
    return true;
}

static bool bx_copy_xattrs_fd(int src_fd, int dest_fd) {
    ssize_t size = flistxattr(src_fd, NULL, 0);
    if (size < 0) {
        if (errno == ENOTSUP || errno == ENOSYS)
            return true;
        return false;
    }
    if (size == 0)
        return true;

    char* list = malloc((size_t)size);
    if (!list)
        return false;

    size = flistxattr(src_fd, list, (size_t)size);
    if (size < 0) {
        free(list);
        return false;
    }

    for (char* name = list; name < list + size; name += strlen(name) + 1) {
        ssize_t val_size = fgetxattr(src_fd, name, NULL, 0);
        if (val_size < 0) {
            if (bx_xattr_error_missing(errno)) {
                continue;
            }
            free(list);
            return false;
        }

        char* value = malloc(val_size > 0 ? (size_t)val_size : 1u);
        if (!value) {
            free(list);
            return false;
        }

        val_size = fgetxattr(src_fd, name, value, (size_t)val_size);
        if (val_size < 0) {
            int err = errno;
            free(value);
            if (bx_xattr_error_missing(err)) {
                continue;
            }
            free(list);
            return false;
        }
        if (fsetxattr(dest_fd, name, value, (size_t)val_size, 0) != 0) {
            free(value);
            free(list);
            return false;
        }
        free(value);
    }

    free(list);
    return true;
}

static bool bx_copy_xattrs_path(const char* src_path, const char* dest_path, bool no_follow) {
    ssize_t size = no_follow ? llistxattr(src_path, NULL, 0) : listxattr(src_path, NULL, 0);
    if (size < 0) {
        if (errno == ENOTSUP || errno == ENOSYS)
            return true;
        return false;
    }
    if (size == 0)
        return true;

    char* list = malloc((size_t)size);
    if (!list)
        return false;

    size = no_follow ? llistxattr(src_path, list, (size_t)size) : listxattr(src_path, list, (size_t)size);
    if (size < 0) {
        free(list);
        return false;
    }

    for (char* name = list; name < list + size; name += strlen(name) + 1) {
        ssize_t val_size = no_follow ? lgetxattr(src_path, name, NULL, 0) : getxattr(src_path, name, NULL, 0);
        if (val_size < 0) {
            if (bx_xattr_error_missing(errno)) {
                continue;
            }
            free(list);
            return false;
        }

        char* value = malloc(val_size > 0 ? (size_t)val_size : 1u);
        if (!value) {
            free(list);
            return false;
        }

        val_size = no_follow ? lgetxattr(src_path, name, value, (size_t)val_size) : getxattr(src_path, name, value, (size_t)val_size);
        if (val_size < 0) {
            int err = errno;
            free(value);
            if (bx_xattr_error_missing(err)) {
                continue;
            }
            free(list);
            return false;
        }
        int rc;
        if (no_follow) {
            rc = lsetxattr(dest_path, name, value, (size_t)val_size, 0);
        }
        else {
            rc = setxattr(dest_path, name, value, (size_t)val_size, 0);
        }
        if (rc != 0) {
            free(value);
            free(list);
            return false;
        }
        free(value);
    }

    free(list);
    return true;
}

bool bx_copy_fd_metadata(int src_fd, int dest_fd, const struct stat* src_stat, unsigned mask) {
    if ((mask & BX_PRESERVE_OWNERSHIP) != 0u) {
        if (fchown(dest_fd, src_stat->st_uid, src_stat->st_gid) != 0) {
            if (!bx_ownership_error_ignorable(errno)) {
                return false;
            }
        }
    }
    if ((mask & BX_PRESERVE_MODE) != 0u) {
        if (fchmod(dest_fd, src_stat->st_mode & 07777u) != 0) {
            return false;
        }
        if (src_fd >= 0) {
            if (!bx_copy_acls_fd(src_fd, dest_fd))
                return false;
        }
    }
    if ((mask & BX_PRESERVE_TIMESTAMPS) != 0u) {
        struct timespec ts[2] = {src_stat->st_atim, src_stat->st_mtim};
        if (futimens(dest_fd, ts) != 0) {
            return false;
        }
    }
    if ((mask & BX_PRESERVE_XATTR) != 0u) {
        if (src_fd >= 0) {
            if (!bx_copy_xattrs_fd(src_fd, dest_fd)) {
                return false;
            }
        }
    }
    return true;
}

bool bx_copy_path_metadata(const char* src_path, const char* dest_path, const struct stat* src_stat, unsigned mask, bool no_follow) {
    if ((mask & BX_PRESERVE_OWNERSHIP) != 0u) {
        if ((no_follow ? lchown(dest_path, src_stat->st_uid, src_stat->st_gid) : chown(dest_path, src_stat->st_uid, src_stat->st_gid)) != 0) {
            if (!bx_ownership_error_ignorable(errno)) {
                return false;
            }
        }
    }

    if (!no_follow && (mask & BX_PRESERVE_MODE) != 0u) {
        if (chmod(dest_path, src_stat->st_mode & 07777u) != 0) {
            return false;
        }
        if (!bx_copy_acls_path(src_path, dest_path, no_follow))
            return false;
    }

    if ((mask & BX_PRESERVE_TIMESTAMPS) != 0u) {
        struct timespec ts[2] = {src_stat->st_atim, src_stat->st_mtim};
        int flags = no_follow ? AT_SYMLINK_NOFOLLOW : 0;
        if (utimensat(AT_FDCWD, dest_path, ts, flags) != 0) {
            return false;
        }
    }

    if ((mask & BX_PRESERVE_XATTR) != 0u) {
        if (!bx_copy_xattrs_path(src_path, dest_path, no_follow)) {
            return false;
        }
    }

    return true;
}
