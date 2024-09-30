#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "copy_metadata.h"

bool bx_copy_fd_metadata(int fd, const struct stat *src_stat, unsigned mask) {
    if ((mask & BX_PRESERVE_OWNERSHIP) != 0u) {
        if (fchown(fd, src_stat->st_uid, src_stat->st_gid) != 0) {
            return false;
        }
    }
    if ((mask & BX_PRESERVE_MODE) != 0u) {
        if (fchmod(fd, src_stat->st_mode & 07777u) != 0) {
            return false;
        }
    }
    if ((mask & BX_PRESERVE_TIMESTAMPS) != 0u) {
        struct timespec ts[2] = {src_stat->st_atim, src_stat->st_mtim};
        if (futimens(fd, ts) != 0) {
            return false;
        }
    }
    return true;
}

bool bx_copy_path_metadata(const char *dest_path, const struct stat *src_stat, unsigned mask, bool no_follow) {
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
    }

    if ((mask & BX_PRESERVE_TIMESTAMPS) != 0u) {
        struct timespec ts[2] = {src_stat->st_atim, src_stat->st_mtim};
        int flags = no_follow ? AT_SYMLINK_NOFOLLOW : 0;
        if (utimensat(AT_FDCWD, dest_path, ts, flags) != 0) {
            return false;
        }
    }

    return true;
}
