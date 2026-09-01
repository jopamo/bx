#ifndef BX_APPLETS_SHELL_ASH_REDIRECTION_H
#define BX_APPLETS_SHELL_ASH_REDIRECTION_H

#include <stdbool.h>
#include <stddef.h>

#include "lib/fd_transaction.h"

struct ash_command;
struct ash_shell;

struct ash_redirection_transaction {
    struct bx_fd_transaction descriptors;
};

void ash_redirection_transaction_init(
    struct ash_redirection_transaction* transaction
);
bool ash_redirection_transaction_active(
    const struct ash_redirection_transaction* transaction
);
int ash_redirection_transaction_apply(
    struct ash_shell* shell,
    const struct ash_command* command,
    struct ash_redirection_transaction* transaction
);
int ash_redirection_transaction_rollback(
    const struct ash_shell* shell,
    struct ash_redirection_transaction* transaction
);
int ash_redirection_transaction_commit(
    const struct ash_shell* shell,
    struct ash_redirection_transaction* transaction
);
int ash_redirections_apply_permanently(
    struct ash_shell* shell,
    const struct ash_command* command
);

bool ash_redirection_parse_fd(
    const struct ash_shell* shell,
    const char* text,
    int* fd
);

#endif /* BX_APPLETS_SHELL_ASH_REDIRECTION_H */
