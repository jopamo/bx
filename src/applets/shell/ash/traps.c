#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/traps.h"

bool ash_traps_invariants(const struct ash_shell* shell) {
    return shell != NULL && (shell->traps == NULL || (shell->traps->owner == shell && (!shell->traps->exit_enabled || shell->traps->exit_action != NULL) &&
                                                      (!shell->traps->exit_running || shell->traps->exit_fired) && (!shell->traps->exit_status_override || shell->traps->exit_running)));
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
        table = calloc(1u, sizeof(*table));
        if (table == NULL) {
            free(replacement);
            return false;
        }
        table->owner = shell;
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

void ash_traps_detach_after_fork(struct ash_shell* shell) {
    assert(ash_traps_invariants(shell));
    if (shell->traps != NULL) {
        shell->traps->exit_enabled = false;
        shell->traps->exit_fired = false;
        shell->traps->exit_running = false;
        shell->traps->exit_status_override = false;
    }
}

void ash_traps_destroy(struct ash_shell* shell) {
    assert(ash_traps_invariants(shell));
    if (shell->traps != NULL) {
        free(shell->traps->exit_action);
        free(shell->traps);
        shell->traps = NULL;
    }
}
