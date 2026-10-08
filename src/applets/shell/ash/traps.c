#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/traps.h"
#include "lib/signal_pending.h"

struct ash_signal_trap {
    struct sigaction original;
    bool valid;
    bool ignored;
    bool enabled;
    char* action;
};

struct ash_signal_traps {
    struct ash_signal_trap entries[NSIG];
    struct bx_signal_pending* pending;
    bool restore_blocked_child;
};

static bool ash_signal_traps_invariants(const struct ash_signal_traps* signals) {
    if (signals == NULL) {
        return true;
    }
    for (int number = 0; number < NSIG; number++) {
        const struct ash_signal_trap* entry = &signals->entries[number];
        if ((!entry->valid && (entry->ignored || entry->enabled || entry->action != NULL)) || (entry->enabled && (entry->action == NULL || signals->pending == NULL || entry->ignored)) ||
            (entry->valid && entry->original.sa_handler == SIG_IGN && !entry->ignored)) {
            return false;
        }
    }
    return true;
}

bool ash_traps_invariants(const struct ash_shell* shell) {
    return shell != NULL &&
           (shell->traps == NULL || (shell->traps->owner == shell && (!shell->traps->exit_enabled || shell->traps->exit_action != NULL) && (!shell->traps->exit_running || shell->traps->exit_fired) &&
                                     (!shell->traps->signal_running || (shell->traps->signals != NULL && shell->traps->signals->pending != NULL)) &&
                                     (!shell->traps->exit_status_override || shell->traps->exit_running) && ash_signal_traps_invariants(shell->traps->signals)));
}

static struct ash_trap_table* ash_trap_table_new(struct ash_shell* shell) {
    struct ash_trap_table* table = calloc(1u, sizeof(*table));
    if (table != NULL) {
        table->owner = shell;
    }
    return table;
}

bool ash_trap_exit_set(struct ash_shell* shell, const char* action) {
    assert(ash_traps_invariants(shell));
    char* replacement = action == NULL ? NULL : strdup(action);
    if (action != NULL && replacement == NULL) {
        return false;
    }
    struct ash_trap_table* table = shell->traps;
    if (table == NULL) {
        if (action == NULL) {
            return true;
        }
        table = ash_trap_table_new(shell);
        if (table == NULL) {
            free(replacement);
            return false;
        }
    }
    free(table->exit_action);
    table->exit_action = replacement;
    table->exit_enabled = action != NULL;
    shell->traps = table;
    assert(ash_traps_invariants(shell));
    return true;
}

const char* ash_trap_exit_action(const struct ash_shell* shell) {
    assert(ash_traps_invariants(shell));
    return shell->traps == NULL ? NULL : shell->traps->exit_action;
}

bool ash_trap_exit_prepare(struct ash_shell* shell, char** action) {
    assert(ash_traps_invariants(shell));
    *action = NULL;
    struct ash_trap_table* table = shell->traps;
    if (table == NULL || !table->exit_enabled || table->exit_fired) {
        return true;
    }
    table->exit_fired = true;
    if (table->exit_action[0] != '\0') {
        *action = strdup(table->exit_action);
        if (*action == NULL) {
            return false;
        }
    }
    table->exit_running = *action != NULL;
    return true;
}

void ash_trap_exit_override_status(struct ash_shell* shell) {
    assert(ash_traps_invariants(shell));
    if (shell->traps != NULL && shell->traps->exit_running) {
        shell->traps->exit_status_override = true;
    }
}

bool ash_trap_exit_finish(struct ash_shell* shell) {
    assert(ash_traps_invariants(shell));
    struct ash_trap_table* table = shell->traps;
    bool override = table != NULL && table->exit_status_override;
    if (table != NULL) {
        table->exit_running = false;
        table->exit_status_override = false;
    }
    return override;
}

int ash_trap_signals_init(struct ash_shell* shell) {
    assert(ash_traps_invariants(shell));
    if (shell->traps != NULL && shell->traps->signals != NULL) {
        return 0;
    }
    struct ash_signal_traps* signals = calloc(1u, sizeof(*signals));
    if (signals == NULL) {
        return ENOMEM;
    }
    for (int number = 1; number < NSIG; number++) {
        struct ash_signal_trap* entry = &signals->entries[number];
        if (sigaction(number, NULL, &entry->original) != 0) {
            int error = errno;
            if (error == EINVAL) {
                continue;
            }
            free(signals);
            return error;
        }
        entry->valid = true;
        entry->ignored = entry->original.sa_handler == SIG_IGN;
    }
    struct ash_trap_table* table = shell->traps;
    if (table == NULL) {
        table = ash_trap_table_new(shell);
        if (table == NULL) {
            free(signals);
            return ENOMEM;
        }
    }
    table->signals = signals;
    shell->traps = table;
    assert(ash_traps_invariants(shell));
    return 0;
}

int ash_trap_signal_limit(void) {
    return NSIG;
}

int ash_traps_enter_signals(struct ash_shell* shell) {
    assert(ash_traps_invariants(shell));
    sigset_t mask;
    if (sigprocmask(SIG_SETMASK, NULL, &mask) < 0)
        return errno;
    if (sigismember(&mask, SIGCHLD) != 1)
        return 0;
    int error = ash_trap_signals_init(shell);
    if (error != 0)
        return error;
    sigset_t child;
    sigemptyset(&child);
    sigaddset(&child, SIGCHLD);
    if (sigprocmask(SIG_UNBLOCK, &child, NULL) < 0)
        return errno;
    shell->traps->signals->restore_blocked_child = true;
    return 0;
}

const char* ash_trap_signal_action(const struct ash_shell* shell, int number) {
    assert(ash_traps_invariants(shell));
    if (number <= 0 || number >= NSIG || shell->traps == NULL || shell->traps->signals == NULL) {
        return NULL;
    }
    const struct ash_signal_trap* entry = &shell->traps->signals->entries[number];
    return !entry->valid ? NULL : entry->ignored ? "" : entry->action;
}

bool ash_trap_signal_supported(const struct ash_shell* shell, int number, const char* action) {
    assert(ash_traps_invariants(shell));
    if (number <= 0 || number >= NSIG || shell->traps == NULL || shell->traps->signals == NULL || ash_shell_policy_has(&shell->policy, ASH_SHELL_POLICY_INTERACTIVE)) {
        return false;
    }
    const struct ash_signal_trap* entry = &shell->traps->signals->entries[number];
    if (number == SIGKILL || number == SIGSTOP || (number == SIGCHLD && entry->original.sa_handler != SIG_IGN)) {
        return false;
    }
    (void)action;
    return entry->valid;
}

int ash_trap_signal_set(struct ash_shell* shell, int number, const char* action) {
    if (!ash_trap_signal_supported(shell, number, action)) {
        return ENOTSUP;
    }
    struct ash_signal_trap* entry = &shell->traps->signals->entries[number];
    if (entry->original.sa_handler == SIG_IGN) {
        return 0;
    }
    bool ignored = action != NULL && action[0] == '\0';
    bool caught = action != NULL && !ignored;
    if (!caught && entry->action == NULL && entry->ignored == ignored) {
        return 0;
    }
    char* replacement_action = caught ? strdup(action) : NULL;
    if (caught && replacement_action == NULL)
        return ENOMEM;
    struct ash_signal_traps* signals = shell->traps->signals;
    struct bx_signal_pending* pending = signals->pending;
    if (caught && pending == NULL) {
        pending = bx_signal_pending_create();
        if (pending == NULL) {
            free(replacement_action);
            return ENOMEM;
        }
    }
    struct sigaction replacement = entry->original;
    if (ignored) {
        replacement = (struct sigaction){.sa_handler = SIG_IGN};
        sigemptyset(&replacement.sa_mask);
    }
    int result = bx_signal_pending_set(pending, number, caught ? NULL : &replacement);
    if (result != 0) {
        int error = errno;
        if (pending != signals->pending)
            bx_signal_pending_destroy(pending);
        free(replacement_action);
        return error;
    }
    signals->pending = pending;
    free(entry->action);
    entry->action = replacement_action;
    entry->enabled = caught;
    entry->ignored = ignored;
    assert(ash_traps_invariants(shell));
    return 0;
}

bool ash_trap_signal_prepare(struct ash_shell* shell, char** action) {
    assert(ash_traps_invariants(shell));
    *action = NULL;
    struct ash_trap_table* table = shell->traps;
    if (table == NULL || table->signal_running || table->signals == NULL || table->signals->pending == NULL)
        return true;
    int number;
    if (bx_signal_pending_take(table->signals->pending, &number) < 0)
        return false;
    if (number == 0)
        return true;
    struct ash_signal_trap* entry = &table->signals->entries[number];
    assert(entry->enabled && entry->action != NULL);
    *action = strdup(entry->action);
    if (*action == NULL)
        return false;
    table->signal_running = true;
    return true;
}

void ash_trap_signal_finish(struct ash_shell* shell) {
    assert(ash_traps_invariants(shell));
    assert(shell->traps != NULL && shell->traps->signal_running);
    shell->traps->signal_running = false;
}

int ash_trap_signal_wait_child(struct ash_shell* shell, pid_t pid, int* number) {
    assert(ash_traps_invariants(shell));
    struct ash_trap_table* table = shell->traps;
    int result = bx_signal_pending_wait_child(table == NULL || table->signal_running || table->signals == NULL ? NULL : table->signals->pending, pid, number);
    return result < 0 && errno == ENOTSUP ? 0 : result;
}

void ash_traps_detach_after_fork(struct ash_shell* shell) {
    assert(ash_traps_invariants(shell));
    if (shell->traps != NULL) {
        shell->traps->exit_enabled = false;
        shell->traps->exit_fired = false;
        shell->traps->exit_running = false;
        shell->traps->exit_status_override = false;
        shell->traps->signal_running = false;
        if (shell->traps->signals != NULL) {
            struct ash_signal_traps* signals = shell->traps->signals;
            bx_signal_pending_detach_after_fork(signals->pending);
            signals->restore_blocked_child = false;
            for (int number = 1; number < NSIG; number++) {
                struct ash_signal_trap* entry = &signals->entries[number];
                entry->enabled = false;
                if (entry->valid && entry->original.sa_handler != SIG_DFL && entry->original.sa_handler != SIG_IGN) {
                    entry->original = (struct sigaction){.sa_handler = SIG_DFL};
                    sigemptyset(&entry->original.sa_mask);
                }
            }
        }
    }
}

void ash_traps_destroy(struct ash_shell* shell) {
    assert(ash_traps_invariants(shell));
    if (shell->traps != NULL) {
        struct ash_signal_traps* signals = shell->traps->signals;
        if (signals != NULL) {
            for (int number = 1; number < NSIG; number++) {
                struct ash_signal_trap* entry = &signals->entries[number];
                if (entry->valid && (entry->enabled || entry->ignored) && entry->original.sa_handler != SIG_IGN && bx_signal_pending_set(signals->pending, number, &entry->original) != 0) {
                    ash_exec_error(shell, "trap reset", errno);
                }
                free(entry->action);
            }
            bx_signal_pending_destroy(signals->pending);
            if (signals->restore_blocked_child) {
                sigset_t child;
                sigemptyset(&child);
                sigaddset(&child, SIGCHLD);
                if (sigprocmask(SIG_BLOCK, &child, NULL) < 0) {
                    int error = errno;
                    (void)sigprocmask(SIG_BLOCK, &child, NULL);
                    ash_exec_error(shell, "signal mask restore", error);
                }
            }
            free(signals);
        }
        free(shell->traps->exit_action);
        free(shell->traps);
        shell->traps = NULL;
    }
}
