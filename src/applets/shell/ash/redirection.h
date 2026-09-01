#ifndef BX_APPLETS_SHELL_ASH_REDIRECTION_H
#define BX_APPLETS_SHELL_ASH_REDIRECTION_H

#include <stdbool.h>
#include <stddef.h>

struct ash_command;
struct ash_shell;

struct ash_saved_fd {
    int target_fd;
    /* Owned CLOEXEC backup, or -1 when the target was originally closed. */
    int saved_fd;
};

struct ash_saved_fds {
    /* Transaction-owned backup descriptors and their backing array. */
    struct ash_saved_fd* items;
    size_t length;
    size_t capacity;
};

void ash_saved_fds_init(struct ash_saved_fds* saved);
void ash_saved_fds_restore(
    const struct ash_shell* shell,
    struct ash_saved_fds* saved
);
void ash_saved_fds_commit(struct ash_saved_fds* saved);
bool ash_redirection_parse_fd(
    const struct ash_shell* shell,
    const char* text,
    int* fd
);
int ash_apply_redirections(
    const struct ash_shell* shell,
    const struct ash_command* command,
    struct ash_saved_fds* saved
);

#endif /* BX_APPLETS_SHELL_ASH_REDIRECTION_H */
