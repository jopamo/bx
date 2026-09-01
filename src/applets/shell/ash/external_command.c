#include <errno.h>

#include "applets/shell/ash/command_resolution.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/external_command.h"
#include "applets/shell/ash/variables.h"
#include "lib/child_runner.h"

static int ash_external_path_search(
    struct ash_shell* shell,
    char** argv,
    const char* command
) {
    const char* path = ash_var_get(shell, "PATH");
    if (path == NULL || path[0] == '\0') {
        path = "/bin:/usr/bin";
    }

    int lookup_error = bx_child_exec_file_argv_in_path(
        command,
        argv,
        path,
        BX_CHILD_PATH_SEARCH_CONTINUE_ON_ERROR
    );
    if (lookup_error == ENOMEM) {
        ash_diag_oom(shell);
        return 126;
    }
    struct ash_command_resolution failure =
        ash_command_resolution_not_found(command, lookup_error);
    if (failure.target.lookup_error == ENOENT ||
        failure.target.lookup_error == ENOTDIR) {
        ash_exec_not_found(shell, failure.command_name);
        return 127;
    }
    ash_exec_error(
        shell,
        failure.command_name,
        failure.target.lookup_error
    );
    return 126;
}

int ash_external_command_exec(
    struct ash_shell* shell,
    char** argv,
    const struct ash_command_resolution* resolution
) {
    if (argv == NULL || argv[0] == NULL) {
        return 0;
    }
    if (!ash_command_resolution_valid(resolution)) {
        ash_exec_error(shell, argv[0], EINVAL);
        return 126;
    }

    switch (resolution->kind) {
        case ASH_COMMAND_PATH_SEARCH:
            return ash_external_path_search(
                shell,
                argv,
                resolution->command_name
            );
        case ASH_COMMAND_HASHED_EXTERNAL:
        case ASH_COMMAND_EXPLICIT_PATH: {
            int error = bx_child_exec_file_argv_exact(
                resolution->target.path,
                argv
            );
            struct ash_command_resolution failure =
                ash_command_resolution_not_found(
                    resolution->command_name,
                    error
                );
            ash_exec_error(
                shell,
                resolution->target.path,
                failure.target.lookup_error
            );
            return error == ENOENT ? 127 : 126;
        }
        case ASH_COMMAND_NOT_FOUND:
            if (resolution->target.lookup_error == ENOENT ||
                resolution->target.lookup_error == ENOTDIR) {
                ash_exec_not_found(shell, resolution->command_name);
                return 127;
            }
            ash_exec_error(
                shell,
                resolution->command_name,
                resolution->target.lookup_error
            );
            return 126;
        case ASH_COMMAND_INVALID:
        case ASH_COMMAND_SPECIAL_BUILTIN:
        case ASH_COMMAND_REGULAR_BUILTIN:
        case ASH_COMMAND_FUNCTION:
        case ASH_COMMAND_BX_APPLET:
            ash_exec_error(shell, resolution->command_name, EINVAL);
            return 126;
    }
    ash_exec_error(shell, resolution->command_name, EINVAL);
    return 126;
}
