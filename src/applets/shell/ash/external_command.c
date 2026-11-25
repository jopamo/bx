#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "applets/shell/ash/command_resolution.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/external_command.h"
#include "applets/shell/ash/variables.h"
#include "lib/child_runner.h"

static char* ash_standard_path(void) {
    size_t size = confstr(_CS_PATH, NULL, 0);
    if (size == 0)
        return NULL;
    char* path = malloc(size);
    if (path != NULL && confstr(_CS_PATH, path, size) == size)
        return path;
    free(path);
    return NULL;
}

int ash_external_command_find(const struct ash_shell* shell, const struct ash_command_resolution* resolution, char** path_out) {
    *path_out = NULL;
    if (resolution->kind == ASH_COMMAND_EXPLICIT_PATH || resolution->kind == ASH_COMMAND_HASHED_EXTERNAL)
        return bx_child_find_executable(resolution->target.path, "",
                                        BX_CHILD_PATH_SEARCH_CONTINUE_ON_ERROR, path_out);
    if (resolution->kind != ASH_COMMAND_PATH_SEARCH)
        return ENOENT;

    char* standard = resolution->default_path ? ash_standard_path() : NULL;
    const char* path = resolution->default_path ? (standard != NULL ? standard : BX_CHILD_DEFAULT_PATH) : ash_var_get(shell, "PATH");
    int error = path != NULL
        ? bx_child_find_executable(resolution->command_name, path,
                                   BX_CHILD_PATH_SEARCH_CONTINUE_ON_ERROR, path_out)
        : ENOENT;
    free(standard);
    return error;
}

static int ash_external_path_search(struct ash_shell* shell, char** argv, const char* command, bool default_path) {
    /*
     * Command assignments have already been applied to the child environment,
     * so that value is the effective PATH for this invocation. Fall back to
     * shell state only for an unexported PATH. Initial defaulting belongs to
     * environment import; an explicitly empty PATH remains an empty segment.
     * command -p overrides only the search path, not the child environment.
     */
    char* standard = default_path ? ash_standard_path() : NULL;
    const char* path = default_path ? (standard != NULL ? standard : BX_CHILD_DEFAULT_PATH) : getenv("PATH");
    if (path == NULL) {
        path = ash_var_get(shell, "PATH");
    }

    int lookup_error = path != NULL ?
        bx_child_exec_file_argv_in_path(
            command,
            argv,
            path,
            BX_CHILD_PATH_SEARCH_CONTINUE_ON_ERROR
        ) :
        ENOENT;
    free(standard);
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

int ash_external_command_exec_exact(
    struct ash_shell* shell,
    const char* command_name,
    const char* executable,
    char** argv
) {
    int error = bx_child_exec_file_argv_exact(executable, argv);
    struct ash_command_resolution failure =
        ash_command_resolution_not_found(command_name, error);
    ash_exec_error(
        shell,
        executable,
        failure.target.lookup_error
    );
    return error == ENOENT ? 127 : 126;
}

int ash_external_command_exec_fd_exact(
    struct ash_shell* shell,
    const char* command_name,
    int executable_fd,
    char** argv
) {
    int error = bx_child_exec_fd_argv_exact(executable_fd, argv);
    struct ash_command_resolution failure =
        ash_command_resolution_not_found(command_name, error);
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
            return ash_external_path_search(shell, argv, resolution->command_name, resolution->default_path);
        case ASH_COMMAND_HASHED_EXTERNAL:
        case ASH_COMMAND_EXPLICIT_PATH:
            return ash_external_command_exec_exact(
                shell,
                resolution->command_name,
                resolution->target.path,
                argv
            );
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
