/* SPDX-License-Identifier: BSD-2-Clause */
#include "io.h"
#include "fat-config.h"
#include "lib/xreadwrite.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) >= 8, "FAT I/O requires 64-bit offsets");

#if BX_FAT_LINUX_BLOCK
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

int bx_fat_open(const char *path, bool writable, off_t create_size, bool follow_symlinks)
{
    struct stat st;
    int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NONBLOCK;
    if (!follow_symlinks)
        flags |= O_NOFOLLOW;
    if (create_size < 0 || (create_size && !writable)) {
        errno = EINVAL;
        return -1;
    }
    /* Without O_CREAT, Linux interprets O_EXCL as an exclusive block claim.
     * It is intentionally not retried without O_EXCL on failure. */
#if BX_FAT_LINUX_BLOCK
    flags |= O_EXCL;
#endif
    if (create_size)
        flags |= O_CREAT | O_EXCL;
    int fd = open(path, flags, 0644);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0)
        goto fail;
    if (!S_ISREG(st.st_mode)) {
#if BX_FAT_LINUX_BLOCK
        if (!create_size && S_ISBLK(st.st_mode))
            return fd;
#endif
        errno = ENOTSUP;
        goto fail;
    }
    if (create_size && ftruncate(fd, create_size) < 0)
        goto fail;
    return fd;
fail:
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
}

int bx_fat_geometry(int fd, uint64_t *bytes, unsigned *sector_size)
{
    struct stat st;
    if (fstat(fd, &st) < 0)
        return -1;
    if (S_ISREG(st.st_mode)) {
        if (st.st_size < 0) {
            errno = EOVERFLOW;
            return -1;
        }
        *bytes = (uint64_t)st.st_size;
        *sector_size = 512;
        return 0;
    }
#if BX_FAT_LINUX_BLOCK
    if (S_ISBLK(st.st_mode)) {
        int sector;
        if (ioctl(fd, BLKGETSIZE64, bytes) < 0 ||
            ioctl(fd, BLKSSZGET, &sector) < 0)
            return -1;
        if (sector <= 0) {
            errno = EINVAL;
            return -1;
        }
        *sector_size = (unsigned)sector;
        return 0;
    }
#endif
    errno = ENOTSUP;
    return -1;
}

ssize_t bx_fat_read(int fd, void *buffer, size_t size)
{
    size_t done = 0;
    if (size > SSIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    while (done < size) {
        ssize_t n = bx_xread(fd, (char *)buffer + done, size - done);
        if (n < 0)
            return -1;
        if (!n)
            break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

ssize_t bx_fat_write(int fd, const void *buffer, size_t size)
{
    if (size > SSIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return bx_xwrite_all(fd, buffer, size) ? (ssize_t)size : -1;
}

static bool valid_range(size_t size, off_t offset)
{
    if (size > SSIZE_MAX || offset < 0 ||
        (uint64_t)offset > INT64_MAX - (uint64_t)size) {
        errno = EOVERFLOW;
        return false;
    }
    return true;
}

ssize_t bx_fat_pread(int fd, void *buffer, size_t size, off_t offset)
{
    size_t done = 0;
    if (!valid_range(size, offset))
        return -1;
    while (done < size) {
        ssize_t n = pread(fd, (char *)buffer + done, size - done, offset + done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        if (!n)
            break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

ssize_t bx_fat_pwrite(int fd, const void *buffer, size_t size, off_t offset)
{
    size_t done = 0;
    if (!valid_range(size, offset))
        return -1;
    while (done < size) {
        ssize_t n = pwrite(fd, (const char *)buffer + done, size - done, offset + done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}
