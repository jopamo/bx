#ifndef BX_COMMON_FD_OPS_H
#define BX_COMMON_FD_OPS_H

#include <stdbool.h>
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

#endif /* BX_COMMON_FD_OPS_H */
