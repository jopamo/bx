#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "applets/shell/ash/applet_command.h"
#include "applets/shell/ash/command_resolution.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/external_command.h"
#include "lib/child_runner.h"
#include "lib/status.h"

static bool ash_applet_command_direct_eligible(
    size_t argc,
    char** argv,
    const struct ash_command_resolution* resolution
) {
    if (!ash_command_resolution_valid(resolution) ||
        resolution->kind != ASH_COMMAND_BX_APPLET ||
        argc == 0u || argc > (size_t)INT_MAX ||
        argv == NULL || argv[0] == NULL || argv[argc] != NULL ||
        resolution->target.bx_applet.applet->main == NULL) {
        return false;
    }

    switch (resolution->target.bx_applet.execution_class) {
        case BX_APPLET_CHILD_IN_PROCESS_SAFE:
        case BX_APPLET_PARENT_SHELL_SAFE:
            return true;
        case BX_APPLET_EXEC_ONLY:
            return false;
    }
    return false;
}

static int ash_applet_command_invalid(
    struct ash_shell* shell,
    char** argv,
    const struct ash_command_resolution* resolution
) {
    const char* command = resolution != NULL &&
            resolution->command_name != NULL ?
        resolution->command_name :
        (argv != NULL && argv[0] != NULL ? argv[0] : "");
    ash_exec_error(shell, command, EINVAL);
    return BX_STATUS_CANNOT_EXEC;
}

int ash_applet_command_prepare_fork(
    struct ash_shell* shell,
    size_t argc,
    char** argv,
    const struct ash_command_resolution* resolution,
    struct ash_applet_child_plan* plan
) {
    if (plan != NULL) {
        *plan = (struct ash_applet_child_plan){0};
    }
    if (!ash_command_resolution_valid(resolution) ||
        resolution->kind != ASH_COMMAND_BX_APPLET ||
        plan == NULL) {
        return ash_applet_command_invalid(shell, argv, resolution);
    }
    if (ash_applet_command_direct_eligible(argc, argv, resolution) &&
        fflush(NULL) != 0) {
        int error = errno != 0 ? errno : EIO;
        ash_exec_error(shell, "write", error);
        return BX_STATUS_ERROR;
    }

    plan->origin_process = getpid();
    return BX_STATUS_OK;
}

int ash_applet_command_run_child(
    struct ash_shell* shell,
    size_t argc,
    char** argv,
    const struct ash_command_resolution* resolution,
    const struct ash_applet_child_plan* plan
) {
    if (!ash_command_resolution_valid(resolution) ||
        resolution->kind != ASH_COMMAND_BX_APPLET ||
        argv == NULL || argv[0] == NULL ||
        plan == NULL || plan->origin_process <= 0 ||
        plan->origin_process == getpid()) {
        return ash_applet_command_invalid(shell, argv, resolution);
    }

    if (ash_applet_command_direct_eligible(argc, argv, resolution)) {
        int signal_error = bx_child_reset_caught_signal_handlers();
        if (signal_error == 0) {
            int status = bx_status_run_applet(
                resolution->target.bx_applet.applet->main,
                (int)argc,
                argv
            );
            if (fflush(NULL) != 0) {
                int error = errno != 0 ? errno : EIO;
                ash_exec_error(shell, "write", error);
                if (status == BX_STATUS_OK) {
                    status = BX_STATUS_ERROR;
                }
            }
            return status;
        }
    }

    /*
     * Failure to reproduce exec signal semantics revokes direct eligibility;
     * the already resolved executable remains the semantic fallback.
     */
    return ash_external_command_exec_exact(
        shell,
        resolution->command_name,
        resolution->target.bx_applet.fallback_path,
        argv
    );
}
