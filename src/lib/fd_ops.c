#define _GNU_SOURCE

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>

#include "lib/fd_ops.h"
#include "bx/diag.h"

bool bx_fd_close(int* p_fd, const char* path, struct bx_diag_ctx* diag) {
    if (p_fd == NULL || *p_fd < 0) {
        return true;
    }

    int fd = *p_fd;
    *p_fd = -1;

    if (close(fd) != 0) {
        if (path != NULL && diag != NULL) {
            bx_perror_path(diag, path);
        }
        return false;
    }

    return true;
}

int bx_fd_open_read(const char* path, struct bx_diag_ctx* diag) {
    int fd = bx_fd_open_cloexec(path, O_RDONLY, 0);
    if (fd < 0) {
        if (diag != NULL) {
            bx_perror_path(diag, path);
        }
    }
    return fd;
}

int bx_fd_open_write(const char* path, int flags, mode_t mode, struct bx_diag_ctx* diag) {
    int fd = bx_fd_open_cloexec(path, O_WRONLY | flags, mode);
    if (fd < 0) {
        if (diag != NULL) {
            bx_perror_path(diag, path);
        }
    }
    return fd;
}

void bx_fd_cleanup(int* p_fd) {
    if (p_fd == NULL || *p_fd < 0) {
        return;
    }
    close(*p_fd);
    *p_fd = -1;
}

int bx_fd_open_cloexec(const char* path, int flags, mode_t mode) {
    return bx_fd_openat_cloexec(AT_FDCWD, path, flags, mode);
}

int bx_fd_openat_cloexec(int dirfd, const char* path, int flags, mode_t mode) {
    int fd;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

#ifdef SYS_openat
    do {
        fd = (int)syscall(SYS_openat, dirfd, path, flags | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EINTR);
#else
    do {
        fd = openat(dirfd, path, flags | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EINTR);
#endif
    return fd;
}

int bx_fd_socket_cloexec(int domain, int type, int protocol) {
    return socket(domain, type | SOCK_CLOEXEC, protocol);
}

int bx_fd_pipe_cloexec(int pipefd[2]) {
    return pipe2(pipefd, O_CLOEXEC);
}

int bx_fd_eventfd_cloexec(unsigned int initval, int flags) {
    return eventfd(initval, flags | EFD_CLOEXEC);
}

int bx_fd_signalfd_cloexec(int fd, const sigset_t* mask, int flags) {
    return signalfd(fd, mask, flags | SFD_CLOEXEC);
}

int bx_fd_dup_cloexec(int oldfd) {
    return fcntl(oldfd, F_DUPFD_CLOEXEC, 0);
}

int bx_fd_dup2_exact(int oldfd, int newfd) {
    return dup2(oldfd, newfd);
}

int bx_fd_open_nofollow_cloexec(const char* path, int flags, mode_t mode) {
    return bx_fd_open_cloexec(path, flags | O_NOFOLLOW, mode);
}

int bx_fd_fstatat_nofollow(int dirfd, const char* path, struct stat* st) {
    if (path == NULL || st == NULL) {
        errno = EINVAL;
        return -1;
    }
    return fstatat(dirfd, path, st, AT_SYMLINK_NOFOLLOW);
}

bool bx_fd_at_name_is_child(const char* name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    return strchr(name, '/') == NULL;
}

static bool bx_fd_at_authority_is_dirfd(int dirfd) {
    return dirfd >= 0;
}

static int bx_fd_at_reject_name(void) {
    errno = EINVAL;
    return -1;
}

static int bx_fd_at_reject_dirfd(void) {
    errno = EBADF;
    return -1;
}

static int bx_fd_at_check_child(int dirfd, const char* name) {
    if (!bx_fd_at_authority_is_dirfd(dirfd)) {
        return bx_fd_at_reject_dirfd();
    }
    if (!bx_fd_at_name_is_child(name)) {
        return bx_fd_at_reject_name();
    }
    return 0;
}

int bx_fd_openat_child(int dirfd, const char* name, int flags, mode_t mode) {
    if (bx_fd_at_check_child(dirfd, name) != 0) {
        return -1;
    }
    return bx_fd_openat_cloexec(dirfd, name, flags, mode);
}

int bx_fd_openat_child_nofollow(int dirfd, const char* name, int flags, mode_t mode) {
    return bx_fd_openat_child(dirfd, name, flags | O_NOFOLLOW, mode);
}

int bx_fd_fstatat_child(int dirfd, const char* name, struct stat* st, int flags) {
    if (st == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (bx_fd_at_check_child(dirfd, name) != 0) {
        return -1;
    }
    return fstatat(dirfd, name, st, flags);
}

int bx_fd_fstatat_child_nofollow(int dirfd, const char* name, struct stat* st) {
    return bx_fd_fstatat_child(dirfd, name, st, AT_SYMLINK_NOFOLLOW);
}

int bx_fd_unlinkat_child(int dirfd, const char* name, int flags) {
    if (bx_fd_at_check_child(dirfd, name) != 0) {
        return -1;
    }
    return unlinkat(dirfd, name, flags);
}

int bx_fd_renameat_child(int olddirfd, const char* oldname, int newdirfd, const char* newname) {
    if (bx_fd_at_check_child(olddirfd, oldname) != 0) {
        return -1;
    }
    if (bx_fd_at_check_child(newdirfd, newname) != 0) {
        return -1;
    }
    return renameat(olddirfd, oldname, newdirfd, newname);
}

int bx_fd_mkdirat_child(int dirfd, const char* name, mode_t mode) {
    if (bx_fd_at_check_child(dirfd, name) != 0) {
        return -1;
    }
    return mkdirat(dirfd, name, mode);
}

int bx_fd_symlinkat_child(const char* target, int linkdirfd, const char* linkname) {
    if (target == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (bx_fd_at_check_child(linkdirfd, linkname) != 0) {
        return -1;
    }
    return symlinkat(target, linkdirfd, linkname);
}

int bx_fd_linkat_child(int olddirfd, const char* oldname, int newdirfd, const char* newname, int flags) {
    if (bx_fd_at_check_child(olddirfd, oldname) != 0) {
        return -1;
    }
    if (bx_fd_at_check_child(newdirfd, newname) != 0) {
        return -1;
    }
    return linkat(olddirfd, oldname, newdirfd, newname, flags);
}
