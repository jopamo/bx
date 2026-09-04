#define _POSIX_C_SOURCE 200809L

#include "lib/daemon_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "lib/child_runner.h"
#include "lib/fd_ops.h"

int bx_daemonize(bool chdir_root, bool preserve_stdout) {
    pid_t pid = bx_child_fork_session_leader();
    if (pid < 0)
        return -1;
    if (pid > 0)
        return 1;
    if (chdir_root && chdir("/") != 0)
        return -1;

    int null_fd = bx_fd_open_cloexec("/dev/null", O_RDWR, 0);
    if (null_fd < 0)
        return -1;
    if (bx_fd_dup2_exact(null_fd, STDIN_FILENO) < 0 ||
        (!preserve_stdout &&
         bx_fd_dup2_exact(null_fd, STDOUT_FILENO) < 0) ||
        bx_fd_dup2_exact(null_fd, STDERR_FILENO) < 0) {
        int saved = errno;
        close(null_fd);
        errno = saved;
        return -1;
    }
    if (null_fd > STDERR_FILENO)
        close(null_fd);
    return 0;
}
