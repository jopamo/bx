#ifndef BX_COMMON_FD_OPS_H
#define BX_COMMON_FD_OPS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
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

/* Open a selected regular file once, without following the final component.
 * The caller owns parent-path resolution. Return an owned readable CLOEXEC fd
 * only if its type/device/inode match expected; fill opened from that fd.
 * O_NONBLOCK prevents a substituted FIFO from hanging before verification. */
int bx_fd_openat_regular_verified(int parent_fd, const char* name,
                                  const struct stat* expected, struct stat* opened);
/* Verify type/device/inode, publishing opened only on success. The two stat
 * pointers may alias. A different inode is ESTALE; syscall errors are retained. */
int bx_fd_fstat_expected(int fd, const struct stat* expected, struct stat* opened);

/* Open a single child for metadata without following symlinks. Prefer a real
 * fd for regular files, directories and FIFOs, verified against a transient
 * O_PATH reference. Symlinks, devices and read-denied objects retain O_PATH;
 * their metadata operations must use supported empty-path primitives. */
int bx_fd_openat_metadata(int parent_fd, const char* name);

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
int bx_fd_socketpair_cloexec(int domain, int type, int protocol, int socketfd[2]);
int bx_fd_pipe_cloexec(int pipefd[2]);
int bx_fd_eventfd_cloexec(unsigned int initval, int flags);
int bx_fd_signalfd_cloexec(int fd, const sigset_t* mask, int flags);
int bx_fd_dup_cloexec(int oldfd);
int bx_fd_dup_cloexec_min(int oldfd, int minimum);
/*
 * Deliberate exec handoff only. Ordinary descriptor duplication must use the
 * CLOEXEC constructors above.
 */
int bx_fd_dup_inheritable_min(int oldfd, int minimum);
int bx_fd_dup2_exact(int oldfd, int newfd);
int bx_fd_set_cloexec(int fd, bool enabled);
int bx_fd_set_nonblocking(int fd, bool enabled);

enum bx_fd_input_ownership {
    BX_FD_INPUT_BORROWED,
    BX_FD_INPUT_OWNED,
};

struct bx_fd_input {
    int fd;
    int relay[2];
    mode_t type;
    bool owned;
};
#define BX_FD_INPUT_INIT {.fd = -1, .relay = {-1, -1}}
/* OWNED transfers the fd on success and requires exclusive ownership of its
 * open file description. BORROWED never changes shared status flags or closes
 * the source. Other borrowed streams must remain nonblocking for their lifetime;
 * storage files, pipes/FIFOs, sockets and Linux null/zero/full devices need no
 * flag changes. Initialization failure leaves the source with its caller. */
int bx_fd_input_init(struct bx_fd_input* input, int fd, enum bx_fd_input_ownership ownership);
/* cancel_fd is a persistent, readable-on-cancellation notification, or -1.
 * Pipe reads use a private nonblocking relay; socket reads use MSG_DONTWAIT.
 * Readiness is never followed by a potentially blocking shared-stream read.
 * Regular-file reads do not promise bounded storage latency. */
ssize_t bx_fd_input_read(struct bx_fd_input* input, void* data, size_t len, int cancel_fd);
void bx_fd_input_close(struct bx_fd_input* input);

/* Non-follow constructors/checks. These force the non-follow bit at the
 * syscall boundary so callers cannot silently downgrade symlink policy. */
int bx_fd_open_nofollow_cloexec(const char* path, int flags, mode_t mode);
int bx_fd_fstatat_nofollow(int dirfd, const char* path, struct stat* st);

/* Path-relative filesystem mutation constructors centralize direct syscall
 * policy for applets that still need full pathname semantics. */
int bx_fd_unlinkat(int dirfd, const char* path, int flags);
int bx_fd_linkat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, int flags);
int bx_fd_symlinkat(const char* target, int linkdirfd, const char* linkpath);
int bx_fd_mkdirat(int dirfd, const char* path, mode_t mode);
int bx_fd_mknodat(int dirfd, const char* path, mode_t mode, dev_t dev);
/* Reject numbers outside Linux's 12-bit major / 20-bit minor encoding. */
bool bx_fd_device_from_numbers(uintmax_t major_number, uintmax_t minor_number, dev_t* device);
/* Discover one data extent within a captured logical size. Return 1 for an
 * extent, 0 for no remaining data, or -1 with errno. Changes the fd offset. */
int bx_fd_next_data_extent(int fd, off_t offset, off_t size, off_t* begin, off_t* end);
int bx_fd_mkfifoat(int dirfd, const char* path, mode_t mode);
int bx_fd_utimensat(int dirfd, const char* path, const struct timespec times[2], int flags);
int bx_fd_fchmod(int fd, mode_t mode);
int bx_fd_fchown(int fd, uid_t owner, gid_t group);
int bx_fd_futimens(int fd, const struct timespec times[2]);
int bx_fd_ftruncate(int fd, off_t length);
int bx_fd_fsync(int fd);
/*
 * Flush buffered output, then fsync the borrowed stream's descriptor.
 * Returns -1 with the first failure's errno; never closes the stream, retries
 * a failed barrier, or substitutes a weaker sync. NULL is rejected with EINVAL.
 * Streams without descriptors fail after flushing.
 */
int bx_fd_stream_flush_sync(FILE* stream);
int bx_fd_fdatasync(int fd);
int bx_fd_syncfs(int fd);
off_t bx_fd_lseek(int fd, off_t offset, int whence);

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
int bx_fd_renameat2(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, unsigned int flags);
int bx_fd_mkdirat_child(int dirfd, const char* name, mode_t mode);
int bx_fd_symlinkat_child(const char* target, int linkdirfd, const char* linkname);
int bx_fd_linkat_child(int olddirfd, const char* oldname, int newdirfd, const char* newname, int flags);

struct bx_fd_staged_file {
    int parent_fd;
    int fd;
    mode_t mode;
    char name[80];
};
#define BX_FD_STAGED_FILE_INIT {.parent_fd = -1, .fd = -1}
/* Borrow the parent, retain a duplicate and create an exclusive sibling.
 * Save its effective creation mode, then restrict access to 0600 before data.
 * The caller owns cleanup on both success and failure. */
int bx_fd_staged_file_begin(struct bx_fd_staged_file* stage, int parent, const char* destination, mode_t mode);
/* Close the writer before renaming. Failure leaves the destination unchanged
 * and retains the private name for discard. No durability barrier is implied. */
int bx_fd_staged_file_publish(struct bx_fd_staged_file* stage, const char* destination);
void bx_fd_staged_file_discard(struct bx_fd_staged_file* stage);

#endif /* BX_COMMON_FD_OPS_H */
