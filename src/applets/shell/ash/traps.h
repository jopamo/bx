#ifndef BX_APPLETS_SHELL_ASH_TRAPS_H
#define BX_APPLETS_SHELL_ASH_TRAPS_H

#include <stdbool.h>
#include <sys/types.h>

struct ash_shell;
struct ash_signal_traps;

struct ash_trap_table {
    struct ash_shell* owner;
    char* exit_action;
    bool exit_enabled;
    bool exit_fired;
    bool exit_running;
    bool exit_status_override;
    bool signal_running;
    struct ash_signal_traps* signals;
};

bool ash_traps_invariants(const struct ash_shell* shell);
bool ash_trap_exit_set(struct ash_shell* shell, const char* action);
const char* ash_trap_exit_action(const struct ash_shell* shell);
/* Claim dispatch once, then copy the action independently of replacements. */
bool ash_trap_exit_prepare(struct ash_shell* shell, char** action);
void ash_trap_exit_override_status(struct ash_shell* shell);
bool ash_trap_exit_finish(struct ash_shell* shell);
/* Capture ingress dispositions before the shell first changes a signal. */
int ash_trap_signals_init(struct ash_shell* shell);
/* Unblock inherited SIGCHLD while retaining ownership of the caller's bit. */
int ash_traps_enter_signals(struct ash_shell* shell);
int ash_trap_signal_limit(void);
bool ash_trap_signal_supported(const struct ash_shell* shell, int number, const char* action);
const char* ash_trap_signal_action(const struct ash_shell* shell, int number);
int ash_trap_signal_set(struct ash_shell* shell, int number, const char* action);
/* Copy one pending action independently of replacements during dispatch. */
bool ash_trap_signal_prepare(struct ash_shell* shell, char** action);
void ash_trap_signal_finish(struct ash_shell* shell);
int ash_trap_signal_wait_child(struct ash_shell* shell, pid_t pid, int* number);
void ash_traps_detach_after_fork(struct ash_shell* shell);
void ash_traps_destroy(struct ash_shell* shell);

#endif
