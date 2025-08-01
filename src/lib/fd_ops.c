#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>

#include "lib/fd_ops.h"
#include "diag.h"

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
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (diag != NULL) {
            bx_perror_path(diag, path);
        }
    }
    return fd;
}

int bx_fd_open_write(const char* path, int flags, mode_t mode, struct bx_diag_ctx* diag) {
    int fd = open(path, O_WRONLY | flags, mode);
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
