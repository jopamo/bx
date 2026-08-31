#include <stdarg.h>
#include <stdio.h>

#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/shell_context.h"
#include "bx/diag.h"

void ash_diag(const struct ash_shell* shell, const char* format, ...) {
    va_list arguments;
    fprintf(stderr, "%s: ", shell->progname);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}

bool ash_diag_oom(const struct ash_shell* shell) {
    ash_diag(shell, "out of memory");
    return false;
}

void ash_exec_error(
    const struct ash_shell* shell,
    const char* subject,
    int error
) {
    ash_diag(shell, "%s: %s", subject, bx_strerror(error));
}

void ash_exec_not_found(
    const struct ash_shell* shell,
    const char* command
) {
    ash_diag(shell, "%s: not found", command);
}
