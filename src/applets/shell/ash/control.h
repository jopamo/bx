#ifndef BX_APPLETS_SHELL_ASH_CONTROL_H
#define BX_APPLETS_SHELL_ASH_CONTROL_H

#include <stdbool.h>

struct ash_shell;

enum ash_control_kind {
    ASH_CONTROL_NONE = 0,
    ASH_CONTROL_BREAK,
    ASH_CONTROL_CONTINUE,
    ASH_CONTROL_RETURN,
};

enum ash_loop_control {
    ASH_LOOP_CONTROL_NONE = 0,
    ASH_LOOP_CONTROL_BREAK,
    ASH_LOOP_CONTROL_CONTINUE,
    ASH_LOOP_CONTROL_PROPAGATE,
};

struct ash_control_state {
    enum ash_control_kind pending;
    unsigned int remaining_levels;
    unsigned int loop_depth;
    int status;
};

void ash_control_enter_loop(struct ash_shell* shell);
void ash_control_leave_loop(struct ash_shell* shell);
void ash_control_request_loop(
    struct ash_shell* shell,
    enum ash_control_kind kind,
    unsigned int levels
);
bool ash_control_pending(const struct ash_shell* shell);
enum ash_loop_control ash_control_consume_loop(struct ash_shell* shell);

#endif /* BX_APPLETS_SHELL_ASH_CONTROL_H */
