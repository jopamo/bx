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

static bool bx_copy_specific_xattr_fd(int src_fd, int dest_fd, const char *name) {
    ssize_t val_size = fgetxattr(src_fd, name, NULL, 0);
    if (val_size < 0) return true;

    char *value = malloc((size_t)val_size);
    if (!value) return false;

    val_size = fgetxattr(src_fd, name, value, (size_t)val_size);
    if (val_size >= 0) {
        fsetxattr(dest_fd, name, value, (size_t)val_size, 0);
    }
    free(value);
    return true;
}

static bool bx_copy_specific_xattr_path(const char *src_path, const char *dest_path, const char *name, bool no_follow) {
    ssize_t val_size = no_follow ? lgetxattr(src_path, name, NULL, 0) : getxattr(src_path, name, NULL, 0);
    if (val_size < 0) return true;

    char *value = malloc((size_t)val_size);
    if (!value) return false;

    val_size = no_follow ? lgetxattr(src_path, name, value, (size_t)val_size) : getxattr(src_path, name, value, (size_t)val_size);
    if (val_size >= 0) {
        if (no_follow) {
            lsetxattr(dest_path, name, value, (size_t)val_size, 0);
        } else {
            setxattr(dest_path, name, value, (size_t)val_size, 0);
        }
    }
    free(value);
    return true;
}

static bool bx_copy_acls_fd(int src_fd, int dest_fd) {
    if (!bx_copy_specific_xattr_fd(src_fd, dest_fd, "system.posix_acl_access")) return false;
    if (!bx_copy_specific_xattr_fd(src_fd, dest_fd, "system.posix_acl_default")) return false;
    return true;
}

static bool bx_copy_acls_path(const char *src_path, const char *dest_path, bool no_follow) {
    if (!bx_copy_specific_xattr_path(src_path, dest_path, "system.posix_acl_access", no_follow)) return false;
    if (!bx_copy_specific_xattr_path(src_path, dest_path, "system.posix_acl_default", no_follow)) return false;
    return true;
}

static bool bx_copy_xattrs_fd(int src_fd, int dest_fd) {
    ssize_t size = flistxattr(src_fd, NULL, 0);
    if (size < 0) {
        if (errno == ENOTSUP || errno == ENOSYS) return true;
        return false;
    }
    if (size == 0) return true;

    char *list = malloc((size_t)size);
    if (!list) return false;

    size = flistxattr(src_fd, list, (size_t)size);
    if (size < 0) {
        free(list);
        return false;
    }

    for (char *name = list; name < list + size; name += strlen(name) + 1) {
        ssize_t val_size = fgetxattr(src_fd, name, NULL, 0);
        if (val_size < 0) continue;

        char *value = malloc((size_t)val_size);
        if (!value) {
            free(list);
            return false;
        }

        val_size = fgetxattr(src_fd, name, value, (size_t)val_size);
        if (val_size >= 0) {
            fsetxattr(dest_fd, name, value, (size_t)val_size, 0);
        }
        free(value);
    }

    free(list);
    return true;
}

static bool bx_copy_xattrs_path(const char *src_path, const char *dest_path, bool no_follow) {
    ssize_t size = no_follow ? llistxattr(src_path, NULL, 0) : listxattr(src_path, NULL, 0);
    if (size < 0) {
        if (errno == ENOTSUP || errno == ENOSYS) return true;
        return false;
    }
    if (size == 0) return true;

    char *list = malloc((size_t)size);
    if (!list) return false;

    size = no_follow ? llistxattr(src_path, list, (size_t)size) : listxattr(src_path, list, (size_t)size);
    if (size < 0) {
        free(list);
        return false;
    }

    for (char *name = list; name < list + size; name += strlen(name) + 1) {
        ssize_t val_size = no_follow ? lgetxattr(src_path, name, NULL, 0) : getxattr(src_path, name, NULL, 0);
        if (val_size < 0) continue;

        char *value = malloc((size_t)val_size);
        if (!value) {
            free(list);
            return false;
        }

        val_size = no_follow ? lgetxattr(src_path, name, value, (size_t)val_size) : getxattr(src_path, name, value, (size_t)val_size);
        if (val_size >= 0) {
            if (no_follow) {
                lsetxattr(dest_path, name, value, (size_t)val_size, 0);
            } else {
                setxattr(dest_path, name, value, (size_t)val_size, 0);
            }
        }
        free(value);
    }

    free(list);
    return true;
}

bool bx_copy_fd_metadata(int src_fd, int dest_fd, const struct stat *src_stat, unsigned mask) {
    if ((mask & BX_PRESERVE_OWNERSHIP) != 0u) {
        if (fchown(dest_fd, src_stat->st_uid, src_stat->st_gid) != 0) {
            return false;
        }
    }
    if ((mask & BX_PRESERVE_MODE) != 0u) {
        if (fchmod(dest_fd, src_stat->st_mode & 07777u) != 0) {
            return false;
        }
        if (src_fd >= 0) {
            if (!bx_copy_acls_fd(src_fd, dest_fd)) return false;
        }
    }
    if ((mask & BX_PRESERVE_TIMESTAMPS) != 0u) {
        struct timespec ts[2] = {src_stat->st_atim, src_stat->st_mtim};
        if (futimens(dest_fd, ts) != 0) {
            return false;
        }
    }
    if ((mask & BX_PRESERVE_CONTEXT) != 0u) {
        if (src_fd >= 0) {
            if (!bx_copy_specific_xattr_fd(src_fd, dest_fd, "security.selinux")) return false;
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

bool bx_copy_path_metadata(const char *src_path, const char *dest_path, const struct stat *src_stat, unsigned mask, bool no_follow) {
    if ((mask & BX_PRESERVE_OWNERSHIP) != 0u) {
        if ((no_follow ? lchown(dest_path, src_stat->st_uid, src_stat->st_gid)
                       : chown(dest_path, src_stat->st_uid, src_stat->st_gid)) != 0) {
            return false;
        }
    }

    if (!no_follow && (mask & BX_PRESERVE_MODE) != 0u) {
        if (chmod(dest_path, src_stat->st_mode & 07777u) != 0) {
            return false;
        }
        if (!bx_copy_acls_path(src_path, dest_path, no_follow)) return false;
    }

    if ((mask & BX_PRESERVE_TIMESTAMPS) != 0u) {
        struct timespec ts[2] = {src_stat->st_atim, src_stat->st_mtim};
        int flags = no_follow ? AT_SYMLINK_NOFOLLOW : 0;
        if (utimensat(AT_FDCWD, dest_path, ts, flags) != 0) {
            return false;
        }
    }

    if ((mask & BX_PRESERVE_CONTEXT) != 0u) {
        if (!bx_copy_specific_xattr_path(src_path, dest_path, "security.selinux", no_follow)) return false;
    }

    if ((mask & BX_PRESERVE_XATTR) != 0u) {
        if (!bx_copy_xattrs_path(src_path, dest_path, no_follow)) {
            return false;
        }
    }

    return true;
}
