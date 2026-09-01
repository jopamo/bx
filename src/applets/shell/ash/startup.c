#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/input.h"
#include "applets/shell/ash/input_execution.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/shell_policy.h"
#include "applets/shell/ash/startup.h"
#include "applets/shell/ash/variables.h"
#include "lib/path_ops.h"

static bool ash_startup_should_read_bashrc(
    const struct ash_shell* shell,
    const struct ash_startup_request* request
) {
    return ash_shell_policy_is_bash(&shell->policy) &&
        getuid() == geteuid() &&
        getgid() == getegid() &&
        ash_shell_policy_has(
            &shell->policy,
            ASH_SHELL_POLICY_INTERACTIVE
        ) &&
        !ash_shell_policy_has(
            &shell->policy,
            ASH_SHELL_POLICY_LOGIN
        ) &&
        request->bashrc != ASH_BASHRC_SUPPRESSED;
}

static char* ash_startup_bashrc_path(
    struct ash_shell* shell,
    const struct ash_startup_request* request
) {
    const char* requested = request->bashrc == ASH_BASHRC_EXPLICIT ?
        request->bashrc_path :
        "~/.bashrc";
    const struct bx_path_tilde_context context = {
        .home = ash_var_get(shell, "HOME"),
        .current_directory = ash_var_get(shell, "PWD"),
        .previous_directory = ash_var_get(shell, "OLDPWD"),
    };
    char* path = bx_path_expand_tilde_dup(requested, &context);
    if (path == NULL) {
        if (errno == ENOMEM) {
            ash_diag_oom(shell);
        }
        else {
            ash_exec_error(
                shell,
                "startup file path",
                errno != 0 ? errno : EINVAL
            );
        }
    }
    return path;
}

enum ash_startup_outcome ash_startup_execute(
    struct ash_shell* shell,
    const struct ash_startup_request* request
) {
    if (shell == NULL || !ash_startup_request_valid(request)) {
        errno = EINVAL;
        if (shell != NULL) {
            ash_diag(shell, "invalid startup request");
        }
        return ASH_STARTUP_FATAL;
    }
    if (!ash_startup_should_read_bashrc(shell, request)) {
        return ASH_STARTUP_CONTINUE;
    }

    char* path = ash_startup_bashrc_path(shell, request);
    if (path == NULL) {
        return ASH_STARTUP_FATAL;
    }
    FILE* stream = fopen(path, "r");
    if (stream == NULL) {
        int error = errno;
        if (error == ENOMEM) {
            ash_diag_oom(shell);
            free(path);
            return ASH_STARTUP_FATAL;
        }
        if (error != ENOENT && error != ENOTDIR) {
            ash_exec_error(shell, path, error);
        }
        free(path);
        return ASH_STARTUP_CONTINUE;
    }

    struct stat metadata;
    if (fstat(fileno(stream), &metadata) != 0) {
        int error = errno;
        ash_exec_error(shell, path, error);
        fclose(stream);
        free(path);
        return ASH_STARTUP_CONTINUE;
    }
    if (S_ISDIR(metadata.st_mode)) {
        ash_exec_error(shell, path, EISDIR);
        fclose(stream);
        free(path);
        return ASH_STARTUP_CONTINUE;
    }

    errno = 0;
    int status = ash_input_execute_stream(
        shell,
        ASH_INPUT_SOURCED_FILE,
        path,
        stream,
        ASH_INPUT_TAKE_STREAM,
        false
    );
    int execution_error = errno;
    free(path);
    if (!shell->should_exit &&
        status == 2 &&
        (execution_error == ENOMEM ||
         execution_error == EOVERFLOW)) {
        return ASH_STARTUP_FATAL;
    }
    return shell->should_exit ?
        ASH_STARTUP_EXIT :
        ASH_STARTUP_CONTINUE;
}
