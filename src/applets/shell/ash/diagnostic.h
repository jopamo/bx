#ifndef BX_APPLETS_SHELL_ASH_DIAGNOSTIC_H
#define BX_APPLETS_SHELL_ASH_DIAGNOSTIC_H

#include <stdbool.h>

struct ash_shell;

void ash_diag(const struct ash_shell* shell, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
bool ash_diag_oom(const struct ash_shell* shell);
void ash_exec_error(
    const struct ash_shell* shell,
    const char* subject,
    int error
);
void ash_exec_not_found(
    const struct ash_shell* shell,
    const char* command
);

#endif /* BX_APPLETS_SHELL_ASH_DIAGNOSTIC_H */
