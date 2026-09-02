#ifndef BX_APPLETS_SHELL_ASH_DIAGNOSTIC_H
#define BX_APPLETS_SHELL_ASH_DIAGNOSTIC_H

#include <stdbool.h>

#include "applets/shell/ash/syntax.h"

struct ash_shell;

/*
 * Runtime diagnostics derive context from the active execution location.
 * Parse callers must pass their error location before parser/input withdrawal.
 */
void ash_diag(const struct ash_shell* shell, const char* format, ...)
    __attribute__((format(printf, 2, 3)));
void ash_diag_parse(
    const struct ash_shell* shell,
    struct ash_source_location location,
    const char* format,
    ...
) __attribute__((format(printf, 3, 4)));
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
