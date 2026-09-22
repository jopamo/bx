#define _GNU_SOURCE

#include "lib/pidfile_ops.h"
#include "lib/fd_ops.h"
#include "lib/path_ops.h"
#include "lib/size_parse.h"
#include "lib/xreadwrite.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

void bx_pidfile_init(struct bx_pidfile *pidfile) {
    if (pidfile)
        *pidfile = (struct bx_pidfile){.parent_fd = -1, .fd = -1};
}

static bool pidfile_name_matches(const struct bx_pidfile* pidfile) {
    struct stat status;
    return bx_fd_fstatat_child_nofollow(pidfile->parent_fd, pidfile->name, &status) == 0 && status.st_dev == pidfile->device && status.st_ino == pidfile->inode;
}

static bool pidfile_is_stale(int fd) {
    char buffer[64];
    size_t used = 0;
    while (used < sizeof(buffer)) {
        ssize_t length = bx_xread(fd, buffer + used, sizeof(buffer) - used);
        if (length < 0)
            return false;
        if (length == 0)
            break;
        used += (size_t)length;
    }
    if (!used || used == sizeof(buffer) || memchr(buffer, '\0', used)) {
        errno = EINVAL;
        return false;
    }
    if (buffer[used - 1] == '\n') {
        used--;
        if (used && buffer[used - 1] == '\r')
            used--;
    }
    buffer[used] = '\0';
    uintmax_t value;
    if (!bx_size_parse_uint(buffer, &value) || value == 0 || value > INT_MAX) {
        errno = EINVAL;
        return false;
    }
    if (kill((pid_t)value, 0) == 0 || errno == EPERM) {
        errno = EEXIST;
        return false;
    }
    return errno == ESRCH;
}

bool bx_pidfile_acquire(struct bx_pidfile *pidfile, const char *path) {
    if (!pidfile || !path || pidfile->active) {
        errno = pidfile && pidfile->active ? EBUSY : EINVAL;
        return false;
    }
    struct bx_pidfile candidate;
    bx_pidfile_init(&candidate);
    candidate.owner = getpid();
    struct bx_path_split split = bx_path_split(path, false);
    candidate.name = strndup(split.basename, split.basename_length);
    char* parent = split.parent_length ? strndup(path, split.parent_length) : strdup(".");
    if (!candidate.name || !parent)
        goto fail_parent;
    if (!bx_fd_at_name_is_child(candidate.name)) {
        errno = EINVAL;
        goto fail_parent;
    }
    candidate.parent_fd = bx_fd_open_cloexec(parent, O_RDONLY | O_DIRECTORY, 0);
    free(parent);
    parent = NULL;
    if (candidate.parent_fd < 0)
        goto fail;

    candidate.fd = bx_fd_openat_cloexec(candidate.parent_fd, candidate.name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK, 0644);
    bool created = candidate.fd >= 0;
    if (!created && errno == EEXIST)
        candidate.fd = bx_fd_openat_cloexec(candidate.parent_fd, candidate.name, O_RDWR | O_NOFOLLOW | O_NONBLOCK, 0);
    if (candidate.fd < 0)
        goto fail;
    struct stat status;
    if (fstat(candidate.fd, &status) != 0)
        goto fail;
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1 || status.st_uid != geteuid()) {
        errno = EINVAL;
        goto fail;
    }
    candidate.device = status.st_dev;
    candidate.inode = status.st_ino;
    /* Only a newly created inode may be removed on an acquisition failure. */
    candidate.active = created;
    if (flock(candidate.fd, LOCK_EX | LOCK_NB) != 0)
        goto fail;
    if (!pidfile_name_matches(&candidate)) {
        errno = ESTALE;
        goto fail;
    }
    if (!created && !pidfile_is_stale(candidate.fd))
        goto fail;
    if (lseek(candidate.fd, 0, SEEK_SET) < 0 || ftruncate(candidate.fd, 0) != 0)
        goto fail;
    char text[32];
    int length = snprintf(text, sizeof(text), "%ld\n", (long)candidate.owner);
    if (length <= 0 || (size_t)length >= sizeof(text) || !bx_xwrite_all(candidate.fd, text, (size_t)length))
        goto fail;
    candidate.active = true;
    *pidfile = candidate;
    return true;

fail_parent:
    free(parent);
fail: {
    int error = errno;
    bx_pidfile_release(&candidate);
    errno = error;
    return false;
}
}

void bx_pidfile_release(struct bx_pidfile *pidfile) {
    if (!pidfile)
        return;
    if (pidfile->active && getpid() == pidfile->owner && pidfile_name_matches(pidfile))
        (void)bx_fd_unlinkat_child(pidfile->parent_fd, pidfile->name, 0);
    /* Do not explicitly unlock: a fork child may share the parent's lock. */
    if (pidfile->fd >= 0)
        close(pidfile->fd);
    if (pidfile->parent_fd >= 0)
        close(pidfile->parent_fd);
    free(pidfile->name);
    bx_pidfile_init(pidfile);
}
