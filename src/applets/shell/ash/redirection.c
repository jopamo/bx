#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets/shell/ash/command.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/redirection.h"
#include "applets/shell/ash/shell_context.h"
#include "lib/fd_ops.h"

void ash_redirection_transaction_init(
    struct ash_redirection_transaction* transaction
) {
    if (transaction != NULL) {
        bx_fd_transaction_init(&transaction->descriptors);
    }
}

bool ash_redirection_transaction_active(
    const struct ash_redirection_transaction* transaction
) {
    return transaction != NULL &&
        bx_fd_transaction_active(&transaction->descriptors);
}

static int ash_redirection_transaction_error(
    const struct ash_shell* shell,
    const char* operation,
    int error
) {
    if (shell == NULL) {
        return 1;
    }
    if (error == ENOMEM) {
        ash_diag_oom(shell);
    }
    else {
        ash_exec_error(shell, operation, error);
    }
    return 1;
}

int ash_redirection_transaction_rollback(
    const struct ash_shell* shell,
    struct ash_redirection_transaction* transaction
) {
    if (transaction == NULL) {
        return ash_redirection_transaction_error(
            shell,
            "redirection rollback",
            EINVAL
        );
    }
    int flush_result = fflush(NULL);
    int flush_error = errno;
    int rollback_result =
        bx_fd_transaction_rollback(&transaction->descriptors);
    int rollback_error = errno;
    if (rollback_result != 0) {
        return ash_redirection_transaction_error(
            shell,
            "redirection rollback",
            rollback_error
        );
    }
    if (flush_result != 0) {
        return ash_redirection_transaction_error(
            shell,
            "redirection flush",
            flush_error != 0 ? flush_error : EIO
        );
    }
    return 0;
}

int ash_redirection_transaction_commit(
    const struct ash_shell* shell,
    struct ash_redirection_transaction* transaction
) {
    if (transaction == NULL ||
        bx_fd_transaction_commit(&transaction->descriptors) != 0) {
        return ash_redirection_transaction_error(
            shell,
            "redirection commit",
            errno
        );
    }
    return 0;
}

static bool ash_redirection_fd_value(const char* text, int* fd) {
    char* end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < 0 || value > INT_MAX) {
        return false;
    }
    *fd = (int)value;
    return true;
}

bool ash_redirection_parse_fd(
    const struct ash_shell* shell,
    const char* text,
    int* fd
) {
    if (!ash_redirection_fd_value(text, fd)) {
        ash_diag(shell, "invalid redirection fd");
        return false;
    }
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
        default:
            errno = EINVAL;
            return -1;
    }
    return bx_fd_open_cloexec(redirection->target, flags, 0666);
}

static bool ash_redirection_collect_descriptor_references(
    const struct ash_shell* shell,
    const struct ash_command* command,
    int** references_out,
    size_t* reference_count_out
) {
    *references_out = NULL;
    *reference_count_out = 0u;
    if (command->redir_count == 0u) {
        return true;
    }
    if (command->redir_count > SIZE_MAX / 2u ||
        command->redir_count * 2u >
            SIZE_MAX / sizeof(**references_out)) {
        return ash_diag_oom(shell);
    }

    int* references = malloc(
        command->redir_count * 2u * sizeof(*references)
    );
    if (references == NULL) {
        return ash_diag_oom(shell);
    }

    size_t reference_count = 0u;
    for (size_t i = 0u; i < command->redir_count; i++) {
        const struct ash_redir* redirection = &command->redirs[i];
        references[reference_count++] = redirection->fd;
        if (redirection->kind == ASH_REDIR_DUP &&
            strcmp(redirection->target, "-") != 0) {
            int source_fd;
            if (ash_redirection_fd_value(
                    redirection->target,
                    &source_fd
                )) {
                references[reference_count++] = source_fd;
            }
        }
    }
    *references_out = references;
    *reference_count_out = reference_count;
    return true;
}

int ash_redirection_transaction_apply(
    struct ash_shell* shell,
    const struct ash_command* command,
    struct ash_redirection_transaction* transaction
) {
    if (shell == NULL || command == NULL || transaction == NULL ||
        ash_redirection_transaction_active(transaction)) {
        errno = EINVAL;
        return ash_redirection_transaction_error(
            shell,
            "redirection transaction",
            errno
        );
    }
    if (fflush(NULL) != 0) {
        int error = errno;
        return ash_redirection_transaction_error(
            shell,
            "redirection flush",
            error != 0 ? error : EIO
        );
    }

    int* references;
    size_t reference_count;
    if (!ash_redirection_collect_descriptor_references(
            shell,
            command,
            &references,
            &reference_count
        )) {
        return 1;
    }
    int begin_result = bx_fd_transaction_begin(
        &shell->redirections,
        &transaction->descriptors,
        references,
        reference_count
    );
    int begin_error = errno;
    free(references);
    if (begin_result != 0) {
        return ash_redirection_transaction_error(
            shell,
            "redirection transaction",
            begin_error
        );
    }

    int status = 0;
    for (size_t i = 0u; i < command->redir_count; i++) {
        const struct ash_redir* redirection = &command->redirs[i];
        if (bx_fd_transaction_save(
                &transaction->descriptors,
                redirection->fd
            ) != 0) {
            status = ash_redirection_transaction_error(
                shell,
                "dup",
                errno
            );
            break;
        }

        if (redirection->kind == ASH_REDIR_DUP) {
            if (strcmp(redirection->target, "-") == 0) {
                if (close(redirection->fd) != 0 && errno != EBADF) {
                    ash_exec_error(shell, "close", errno);
                    status = 1;
                    break;
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
                status = 1;
                break;
            }
            continue;
        }

        int fd = ash_open_redirection(shell, redirection);
        if (fd < 0) {
            ash_exec_error(shell, redirection->target, errno);
            status = 1;
            break;
        }
        if (fd == redirection->fd) {
            if (bx_fd_set_cloexec(fd, false) != 0) {
                int error = errno;
                close(fd);
                ash_exec_error(shell, "fcntl", error);
                status = 1;
                break;
            }
        }
        else {
            if (bx_fd_dup2_exact(fd, redirection->fd) < 0) {
                int error = errno;
                close(fd);
                ash_exec_error(shell, "dup2", error);
                status = 1;
                break;
            }
            close(fd);
        }
    }
    if (status != 0) {
        (void)ash_redirection_transaction_rollback(shell, transaction);
    }
    return status;
}

int ash_redirections_apply_permanently(
    struct ash_shell* shell,
    const struct ash_command* command
) {
    struct ash_redirection_transaction transaction;
    ash_redirection_transaction_init(&transaction);
    if (ash_redirection_transaction_apply(
            shell,
            command,
            &transaction
        ) != 0) {
        return 1;
    }
    return ash_redirection_transaction_commit(shell, &transaction);
}
