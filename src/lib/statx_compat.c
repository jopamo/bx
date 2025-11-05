#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include "lib/statx_compat.h"

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0
#endif

#ifndef AT_NO_AUTOMOUNT
#define AT_NO_AUTOMOUNT 0
#endif

#ifndef AT_STATX_DONT_SYNC
#define AT_STATX_DONT_SYNC 0
#endif

int bx_statx_flags = AT_NO_AUTOMOUNT | AT_STATX_DONT_SYNC;

#ifdef SYS_statx
static int bx_statx_fill(int dirfd, const char* pathname, int flags, unsigned int mask, struct stat* st) {
    struct statx stx;
    int ret = syscall(SYS_statx, dirfd, pathname, flags, mask, &stx);

    if (ret < 0) {
        return ret;
    }

    st->st_dev = makedev(stx.stx_dev_major, stx.stx_dev_minor);
    st->st_rdev = makedev(stx.stx_rdev_major, stx.stx_rdev_minor);
    st->st_ino = stx.stx_ino;
    st->st_mode = stx.stx_mode;
    st->st_nlink = stx.stx_nlink;
    st->st_uid = stx.stx_uid;
    st->st_gid = stx.stx_gid;
    st->st_size = stx.stx_size;
    st->st_blksize = stx.stx_blksize;
    st->st_blocks = stx.stx_blocks;
    st->st_atim.tv_sec = stx.stx_atime.tv_sec;
    st->st_atim.tv_nsec = stx.stx_atime.tv_nsec;
    st->st_mtim.tv_sec = stx.stx_mtime.tv_sec;
    st->st_mtim.tv_nsec = stx.stx_mtime.tv_nsec;
    st->st_ctim.tv_sec = stx.stx_ctime.tv_sec;
    st->st_ctim.tv_nsec = stx.stx_ctime.tv_nsec;
    return 0;
}
#endif

int bx_statx_stat(const char* pathname, unsigned int mask, struct stat* st) {
#ifdef SYS_statx
    int dirfd = (pathname != NULL && pathname[0] == '/') ? 0 : AT_FDCWD;
    if (bx_statx_fill(dirfd, pathname, bx_statx_flags, mask, st) == 0) {
        return 0;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return -1;
    }
#else
    (void)mask;
#endif
    return stat(pathname, st);
}

int bx_statx_fstat(int fd, unsigned int mask, struct stat* st) {
#ifdef SYS_statx
    if (bx_statx_fill(fd, "", AT_EMPTY_PATH | bx_statx_flags, mask, st) == 0) {
        return 0;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return -1;
    }
#else
    (void)mask;
#endif
    return fstat(fd, st);
}

int bx_statx_lstat(const char* pathname, unsigned int mask, struct stat* st) {
#ifdef SYS_statx
    int dirfd = (pathname != NULL && pathname[0] == '/') ? 0 : AT_FDCWD;
    if (bx_statx_fill(dirfd, pathname, AT_SYMLINK_NOFOLLOW | bx_statx_flags, mask, st) == 0) {
        return 0;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return -1;
    }
#else
    (void)mask;
#endif
    return lstat(pathname, st);
}
