#include "applets/shell/ash/control.h"
#include "applets/shell/ash/shell_context.h"

void ash_control_enter_loop(struct ash_shell* shell) {
    shell->control.loop_depth++;
}

void ash_control_leave_loop(struct ash_shell* shell) {
    if (shell->control.loop_depth != 0u) {
        shell->control.loop_depth--;
    }
}

void ash_control_request_loop(
    struct ash_shell* shell,
    enum ash_control_kind kind,
    unsigned int levels
) {
    if ((kind != ASH_CONTROL_BREAK && kind != ASH_CONTROL_CONTINUE) ||
        levels == 0u || shell->control.loop_depth == 0u) {
        return;
    }
    shell->control.pending = kind;
    shell->control.remaining_levels =
        levels < shell->control.loop_depth ?
            levels : shell->control.loop_depth;
}

bool ash_control_pending(const struct ash_shell* shell) {
    return shell->control.pending != ASH_CONTROL_NONE;
}

enum ash_loop_control ash_control_consume_loop(struct ash_shell* shell) {
    if (shell->control.pending != ASH_CONTROL_BREAK &&
        shell->control.pending != ASH_CONTROL_CONTINUE) {
        return ASH_LOOP_CONTROL_NONE;
    }
    if (shell->control.remaining_levels > 1u) {
        shell->control.remaining_levels--;
        return ASH_LOOP_CONTROL_PROPAGATE;
    }

    enum ash_control_kind pending = shell->control.pending;
    shell->control.pending = ASH_CONTROL_NONE;
    shell->control.remaining_levels = 0u;
    return pending == ASH_CONTROL_BREAK ?
        ASH_LOOP_CONTROL_BREAK : ASH_LOOP_CONTROL_CONTINUE;
}

void ash_control_enter_function(struct ash_shell* shell) {
    shell->control.function_depth++;
}

void ash_control_leave_function(struct ash_shell* shell) {
    if (shell->control.function_depth != 0u) {
        shell->control.function_depth--;
    }
}

bool ash_control_return_allowed(const struct ash_shell* shell) {
    if (shell->control.function_depth != 0u) {
        return true;
    }
    for (const struct ash_execution_frame* frame = shell->execution_frames; frame != NULL; frame = frame->previous) {
        if (frame->kind == ASH_EXECUTION_SOURCE_FRAME && frame->source_kind == ASH_INPUT_SOURCED_FILE) {
            return true;
        }
    }
    return false;
}

bool ash_control_request_return(struct ash_shell* shell, int status) {
    if (!ash_control_return_allowed(shell)) {
        return false;
    }
    shell->control.pending = ASH_CONTROL_RETURN;
    shell->control.remaining_levels = 0u;
    shell->control.status = status;
    return true;
}

bool ash_control_consume_return(struct ash_shell* shell, int* status) {
    if (shell->control.pending != ASH_CONTROL_RETURN) {
        return false;
    }
    *status = shell->control.status;
    shell->control.pending = ASH_CONTROL_NONE;
    shell->control.status = 0;
    return true;
}

void ash_control_discard_unit(struct ash_shell* shell, int status) {
    shell->control.pending = ASH_CONTROL_DISCARD_UNIT;
    shell->control.remaining_levels = 0u;
    shell->control.status = status;
}

bool ash_control_unit_discarded(const struct ash_shell* shell) {
    return shell->control.pending == ASH_CONTROL_DISCARD_UNIT;
}

bool ash_control_consume_unit_discard(struct ash_shell* shell, int* status) {
    if (!ash_control_unit_discarded(shell)) {
        return false;
    }
    *status = shell->control.status;
    shell->control.pending = ASH_CONTROL_NONE;
    shell->control.status = 0;
    return true;
}
