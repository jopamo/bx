#ifndef BX_COMMON_FD_OPS_H
#define BX_COMMON_FD_OPS_H

#include <stdbool.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "bx/diag.h"

/* bx_fd_close: close *p_fd if >= 0, sets *p_fd to -1.
 * Reports error to diag if path is non-NULL.
 * Returns true if successful or already closed. */
bool bx_fd_close(int* p_fd, const char* path, struct bx_diag_ctx* diag);

/* bx_fd_open_read: wrapper for open(O_RDONLY).
 * Reports error to diag. returns fd or -1. */
int bx_fd_open_read(const char* path, struct bx_diag_ctx* diag);

/* bx_fd_open_write: wrapper for open(O_WRONLY | flags).
 * Reports error to diag. returns fd or -1. */
int bx_fd_open_write(const char* path, int flags, mode_t mode, struct bx_diag_ctx* diag);

/* bx_fd_cleanup: close *p_fd if >= 0, no error reporting.
 * sets *p_fd to -1. useful for fail blocks. */
void bx_fd_cleanup(int* p_fd);

/* CLOEXEC-enforcing constructors. These are the shared authority point for
 * creating process-local descriptors that must not leak across exec. */
int bx_fd_open_cloexec(const char* path, int flags, mode_t mode);
int bx_fd_openat_cloexec(int dirfd, const char* path, int flags, mode_t mode);
int bx_fd_socket_cloexec(int domain, int type, int protocol);
int bx_fd_pipe_cloexec(int pipefd[2]);
int bx_fd_eventfd_cloexec(unsigned int initval, int flags);
int bx_fd_signalfd_cloexec(int fd, const sigset_t* mask, int flags);
int bx_fd_dup_cloexec(int oldfd);
int bx_fd_dup2_exact(int oldfd, int newfd);

/* Non-follow constructors/checks. These force the non-follow bit at the
 * syscall boundary so callers cannot silently downgrade symlink policy. */
int bx_fd_open_nofollow_cloexec(const char* path, int flags, mode_t mode);
int bx_fd_fstatat_nofollow(int dirfd, const char* path, struct stat* st);

/* fd-relative child helpers are for recursive mutation/walk frames.
 * They require an already-open directory fd and a single child name.
 * Empty names, "."/"..", names containing '/', and AT_FDCWD-style
 * authority fallback fail closed before reaching the syscall. */
bool bx_fd_at_name_is_child(const char* name);
int bx_fd_openat_child(int dirfd, const char* name, int flags, mode_t mode);
int bx_fd_openat_child_nofollow(int dirfd, const char* name, int flags, mode_t mode);
int bx_fd_fstatat_child(int dirfd, const char* name, struct stat* st, int flags);
int bx_fd_fstatat_child_nofollow(int dirfd, const char* name, struct stat* st);
int bx_fd_unlinkat_child(int dirfd, const char* name, int flags);
int bx_fd_renameat_child(int olddirfd, const char* oldname, int newdirfd, const char* newname);
int bx_fd_mkdirat_child(int dirfd, const char* name, mode_t mode);
int bx_fd_symlinkat_child(const char* target, int linkdirfd, const char* linkname);
int bx_fd_linkat_child(int olddirfd, const char* oldname, int newdirfd, const char* newname, int flags);

#endif /* BX_COMMON_FD_OPS_H */
