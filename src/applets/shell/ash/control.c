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
