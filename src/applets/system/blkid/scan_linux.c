/* SPDX-License-Identifier: BSD-2-Clause */
/* glibc exposes O_PATH only with _GNU_SOURCE; musl exposes it directly. */
#define _GNU_SOURCE

#include "scan.h"

#include <errno.h>

#ifdef __linux__
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>

#include "lib/dir_path.h"
#include "lib/fd_ops.h"
#include "lib/xreadwrite.h"

/* Linux block uevents have a 2 KiB limit. Allow room for the sysfs line
 * separators; never accept a devnode name without its terminating newline. */
static char *read_devname(int sysfs, const char *entry, char *path, size_t size)
{
    char uevent[64];
    int len = snprintf(uevent, sizeof(uevent), "%s/uevent", entry);
    if (len < 0 || (size_t)len >= sizeof(uevent))
        return NULL;
    int fd = bx_fd_openat_cloexec(sysfs, uevent, O_RDONLY | O_NONBLOCK | O_NOFOLLOW, 0);
    if (fd < 0)
        return NULL;
    ssize_t n = bx_xread(fd, path, size - 1);
    close(fd);
    if (n <= 0 || memchr(path, 0, n))
        return NULL;
    path[n] = 0;

    /* Use the first kernel DEVNAME field, not a field embedded in a later
     * property such as a partition name. */
    char *name = strncmp(path, "DEVNAME=", 8) == 0 ? path : strstr(path, "\nDEVNAME=");
    if (!name)
        return NULL;
    name += (*name == '\n') ? 9 : 8;
    char *end = strchr(name, '\n');
    if (!end || end == name || name[0] == '/' || end[-1] == '/')
        return NULL;
    *end = 0;
    if (name + strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                            "0123456789_-.!+/") != end)
        return NULL;
    return name;
}

static int open_device(int dev, const char *name, const char *device_id)
{
    char *leaf;
    int parent = bx_dir_path_open_parent(dev, name, false, 0, &leaf);
    if (parent < 0)
        return -1;

    int fd = -1;
    struct stat named, opened;
    if (bx_fd_fstatat_child_nofollow(parent, leaf, &named) == 0 && S_ISBLK(named.st_mode)) {
        char id[64];
        snprintf(id, sizeof(id), "%u:%u", major(named.st_rdev), minor(named.st_rdev));
        if (strcmp(id, device_id) == 0) {
            fd = bx_fd_openat_child_nofollow(parent, leaf,
                O_RDONLY | O_NONBLOCK | O_NOCTTY, 0);
            if (fd >= 0 && (bx_fd_fstat_expected(fd, &named, &opened) != 0 ||
                            opened.st_rdev != named.st_rdev))
                bx_fd_cleanup(&fd);
        }
    }
    free(leaf);
    close(parent);
    return fd;
}

int bx_blkid_scan(bool (*visit)(int, const char *, void *), void *ctx)
{
    int sysfs = bx_fd_open_nofollow_cloexec("/sys/dev/block", O_RDONLY | O_DIRECTORY, 0);
    if (sysfs < 0)
        return -1;
    DIR *dir = fdopendir(sysfs);
    if (!dir) {
        int error = errno;
        close(sysfs);
        errno = error;
        return -1;
    }
    int dev = bx_fd_open_nofollow_cloexec("/dev", O_PATH | O_DIRECTORY, 0);
    int error = dev < 0 ? errno : 0;
    if (dev >= 0) {
        for (;;) {
            errno = 0;
            struct dirent *entry = readdir(dir);
            if (!entry) {
                error = errno;
                break;
            }
            if (entry->d_name[0] == '.')
                continue;
            char path[5 + 4096] = "/dev/";
            char *name = read_devname(sysfs, entry->d_name, path + 5, sizeof(path) - 5);
            if (!name)
                continue;
            int fd = open_device(dev, name, entry->d_name);
            if (fd < 0)
                continue; /* Missing, inaccessible or changed device node. */
            memmove(path + 5, name, strlen(name) + 1);
            bool more = visit(fd, path, ctx);
            close(fd);
            if (!more)
                break;
        }
        close(dev);
    }
    closedir(dir);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}
#else
int bx_blkid_scan(bool (*visit)(int, const char *, void *), void *ctx)
{
    (void)visit;
    (void)ctx;
    errno = ENOTSUP;
    return -1;
}
#endif
