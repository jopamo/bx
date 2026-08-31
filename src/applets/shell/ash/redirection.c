#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets/shell/ash/command.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/redirection.h"
#include "applets/shell/ash/shell_context.h"
#include "lib/fd_ops.h"

void ash_saved_fds_init(struct ash_saved_fds* saved) {
    *saved = (struct ash_saved_fds){0};
}

static bool ash_saved_fds_has_target(
    const struct ash_saved_fds* saved,
    int fd
) {
    for (size_t i = 0u; i < saved->length; i++) {
        if (saved->items[i].target_fd == fd) {
            return true;
        }
    }
    return false;
}

static bool ash_saved_fds_push(
    const struct ash_shell* shell,
    struct ash_saved_fds* saved,
    int target_fd,
    int saved_fd
) {
    if (saved->length == saved->capacity) {
        size_t capacity = saved->capacity == 0u ?
            4u : saved->capacity * 2u;
        if (capacity < saved->capacity ||
            capacity > SIZE_MAX / sizeof(*saved->items)) {
            return ash_diag_oom(shell);
        }
        struct ash_saved_fd* items = realloc(
            saved->items,
            capacity * sizeof(*items)
        );
        if (items == NULL) {
            return ash_diag_oom(shell);
        }
        saved->items = items;
        saved->capacity = capacity;
    }
    saved->items[saved->length++] = (struct ash_saved_fd){
        .target_fd = target_fd,
        .saved_fd = saved_fd,
    };
    return true;
}

void ash_saved_fds_restore(
    const struct ash_shell* shell,
    struct ash_saved_fds* saved
) {
    while (saved->length != 0u) {
        struct ash_saved_fd item = saved->items[--saved->length];
        if (item.saved_fd >= 0) {
            if (bx_fd_dup2_exact(item.saved_fd, item.target_fd) < 0) {
                ash_exec_error(shell, "dup2", errno);
            }
            close(item.saved_fd);
        }
        else {
            close(item.target_fd);
        }
    }
    free(saved->items);
    *saved = (struct ash_saved_fds){0};
}

void ash_saved_fds_destroy(struct ash_saved_fds* saved) {
    for (size_t i = 0u; i < saved->length; i++) {
        if (saved->items[i].saved_fd >= 0) {
            close(saved->items[i].saved_fd);
        }
    }
    free(saved->items);
    *saved = (struct ash_saved_fds){0};
}

bool ash_redirection_parse_fd(
    const struct ash_shell* shell,
    const char* text,
    int* fd
) {
    char* end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < 0 || value > INT_MAX) {
        ash_diag(shell, "invalid redirection fd");
        return false;
    }
    *fd = (int)value;
    return true;
}

static int ash_open_redirection(
    const struct ash_shell* shell,
    const struct ash_redir* redirection
) {
    if (redirection->kind == ASH_REDIR_OUT &&
        (shell->options & ASH_SHELL_OPTION_NOCLOBBER) != 0u) {
        int fd = bx_fd_open_cloexec(
            redirection->target,
            O_WRONLY | O_CREAT | O_EXCL,
            0666
        );
        if (fd >= 0 || errno != EEXIST) {
            return fd;
        }
        fd = bx_fd_open_cloexec(redirection->target, O_WRONLY, 0);
        if (fd < 0) {
            return -1;
        }
        struct stat status;
        if (fstat(fd, &status) != 0 || S_ISREG(status.st_mode)) {
            int error = errno;
            close(fd);
            errno = error != 0 ? error : EEXIST;
            return -1;
        }
        return fd;
    }

    int flags;
    switch (redirection->kind) {
        case ASH_REDIR_IN:
            flags = O_RDONLY;
            break;
        case ASH_REDIR_OUT:
        case ASH_REDIR_CLOBBER:
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            break;
        case ASH_REDIR_APPEND:
            flags = O_WRONLY | O_CREAT | O_APPEND;
            break;
        case ASH_REDIR_READWRITE:
            flags = O_RDWR | O_CREAT;
            break;
        case ASH_REDIR_DUP:
            errno = EINVAL;
            return -1;
    }
    return bx_fd_open_cloexec(redirection->target, flags, 0666);
}

int ash_apply_redirections(
    const struct ash_shell* shell,
    const struct ash_command* command,
    struct ash_saved_fds* saved
) {
    int minimum_saved_fd = 10;
    for (size_t i = 0u; i < command->redir_count; i++) {
        if (command->redirs[i].fd >= minimum_saved_fd &&
            command->redirs[i].fd < INT_MAX) {
            minimum_saved_fd = command->redirs[i].fd + 1;
        }
    }

    for (size_t i = 0u; i < command->redir_count; i++) {
        const struct ash_redir* redirection = &command->redirs[i];
        if (saved != NULL &&
            !ash_saved_fds_has_target(saved, redirection->fd)) {
            int duplicate = bx_fd_dup_cloexec_min(
                redirection->fd,
                minimum_saved_fd
            );
            if (duplicate < 0 && errno != EBADF) {
                ash_exec_error(shell, "dup", errno);
                return 1;
            }
            if (!ash_saved_fds_push(
                    shell,
                    saved,
                    redirection->fd,
                    duplicate
                )) {
                if (duplicate >= 0) {
                    close(duplicate);
                }
                return 1;
            }
        }

        if (redirection->kind == ASH_REDIR_DUP) {
            if (strcmp(redirection->target, "-") == 0) {
                if (close(redirection->fd) != 0 && errno != EBADF) {
                    ash_exec_error(shell, "close", errno);
                    return 1;
                }
                continue;
            }
            int source_fd;
            if (!ash_redirection_parse_fd(
                    shell,
                    redirection->target,
                    &source_fd
                ) ||
                bx_fd_dup2_exact(source_fd, redirection->fd) < 0) {
                if (errno != 0) {
                    ash_exec_error(shell, redirection->target, errno);
                }
                return 1;
            }
            continue;
        }

        int fd = ash_open_redirection(shell, redirection);
        if (fd < 0) {
            ash_exec_error(shell, redirection->target, errno);
            return 1;
        }
        if (fd == redirection->fd) {
            if (bx_fd_set_cloexec(fd, false) != 0) {
                int error = errno;
                close(fd);
                ash_exec_error(shell, "fcntl", error);
                return 1;
            }
        }
        else {
            if (bx_fd_dup2_exact(fd, redirection->fd) < 0) {
                int error = errno;
                close(fd);
                ash_exec_error(shell, "dup2", error);
                return 1;
            }
            close(fd);
        }
    }
    return 0;
}
