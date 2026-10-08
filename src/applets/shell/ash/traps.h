#ifndef BX_APPLETS_SHELL_ASH_TRAPS_H
#define BX_APPLETS_SHELL_ASH_TRAPS_H

#include <stdbool.h>

struct ash_shell;

struct ash_trap_table {
    struct ash_shell* owner;
    char* exit_action;
    bool exit_enabled;
    bool exit_fired;
    bool exit_running;
    bool exit_status_override;
};

bool ash_traps_invariants(const struct ash_shell* shell);
bool ash_trap_exit_set(struct ash_shell* shell, const char* action);
const char* ash_trap_exit_action(const struct ash_shell* shell);
/* Claim dispatch once, then copy the action independently of replacements. */
bool ash_trap_exit_prepare(struct ash_shell* shell, char** action);
void ash_trap_exit_override_status(struct ash_shell* shell);
bool ash_trap_exit_finish(struct ash_shell* shell);
void ash_traps_detach_after_fork(struct ash_shell* shell);
void ash_traps_destroy(struct ash_shell* shell);

#endif
