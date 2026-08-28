#define _POSIX_C_SOURCE 200809L

#include "lib/pidfile_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lib/fd_ops.h"
#include "lib/xreadwrite.h"

void bx_pidfile_init(struct bx_pidfile *pidfile) {
    if (pidfile != NULL)
        memset(pidfile, 0, sizeof(*pidfile));
}

static bool bx_pidfile_existing_is_live(const char *path) {
    int fd = bx_fd_open_cloexec(path, O_RDONLY, 0);
    if (fd < 0)
        return false;
    char buffer[64];
    ssize_t length = read(fd, buffer, sizeof(buffer) - 1u);
    close(fd);
    if (length <= 0)
        return false;
    buffer[length] = '\0';
    char *end = NULL;
    errno = 0;
    long value = strtol(buffer, &end, 10);
    if (errno != 0 || end == buffer || value <= 1)
        return false;
    return kill((pid_t)value, 0) == 0 || errno == EPERM;
}

bool bx_pidfile_acquire(struct bx_pidfile *pidfile, const char *path) {
    if (pidfile == NULL || path == NULL) {
        errno = EINVAL;
        return false;
    }
    for (unsigned attempt = 0u; attempt < 2u; attempt++) {
        int fd = bx_fd_open_cloexec(
            path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            char text[64];
            int length = snprintf(text, sizeof(text), "%ld\n", (long)getpid());
            bool ok = length > 0 && (size_t)length < sizeof(text) &&
                bx_xwrite_all(fd, text, (size_t)length);
            struct stat status;
            if (ok)
                ok = fstat(fd, &status) == 0;
            int saved = errno;
            close(fd);
            if (!ok) {
                unlink(path);
                errno = saved;
                return false;
            }
            pidfile->path = strdup(path);
            if (pidfile->path == NULL) {
                unlink(path);
                return false;
            }
            pidfile->owner = getpid();
            pidfile->device = status.st_dev;
            pidfile->inode = status.st_ino;
            pidfile->active = true;
            return true;
        }
        if (errno != EEXIST || bx_pidfile_existing_is_live(path))
            return false;
        if (unlink(path) != 0 && errno != ENOENT)
            return false;
    }
    errno = EEXIST;
    return false;
}

void bx_pidfile_release(struct bx_pidfile *pidfile) {
    if (pidfile == NULL)
        return;
    if (pidfile->active && getpid() == pidfile->owner) {
        struct stat status;
        if (lstat(pidfile->path, &status) == 0 &&
            status.st_dev == pidfile->device &&
            status.st_ino == pidfile->inode)
            (void)unlink(pidfile->path);
    }
    free(pidfile->path);
    bx_pidfile_init(pidfile);
}
