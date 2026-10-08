#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "applets/shell/ash/command.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/trap_builtin.h"
#include "applets/shell/ash/traps.h"
#include "lib/output_quote.h"

static bool ash_trap_is_exit(const char* specification) {
    return strcmp(specification, "EXIT") == 0 || (specification[0] != '\0' && strspn(specification, "0") == strlen(specification));
}

static int ash_trap_print_exit(struct ash_shell* shell) {
    const char* action = ash_trap_exit_action(shell);
    if (action == NULL) {
        return 0;
    }
    if (fputs("trap -- ", stdout) == EOF || !bx_output_quote_write_single(stdout, action) || fputs(" EXIT\n", stdout) == EOF || fflush(stdout) == EOF) {
        ash_exec_error(shell, "trap", errno != 0 ? errno : EIO);
        return 1;
    }
    return 0;
}

int ash_trap_builtin(struct ash_shell* shell, const struct ash_command* command) {
    size_t operand = 1u;
    bool print = false;
    while (operand < command->word_count && command->words[operand][0] == '-' && command->words[operand][1] != '\0') {
        const char* option = command->words[operand++];
        if (strcmp(option, "--") == 0) {
            break;
        }
        for (size_t i = 1u; option[i] != '\0'; i++) {
            if (option[i] != 'p') {
                ash_diag(shell, "trap: unsupported option '-%c'", option[i]);
                return 2;
            }
            print = true;
        }
    }
    if (operand == command->word_count) {
        return ash_trap_print_exit(shell);
    }
    const char* action = NULL;
    bool shorthand = ash_trap_is_exit(command->words[operand]) && (operand + 1u == command->word_count || strcmp(command->words[operand], "EXIT") != 0);
    if (!print && !shorthand) {
        action = command->words[operand++];
        if (strcmp(action, "-") == 0) {
            action = NULL;
        }
        if (operand == command->word_count) {
            ash_diag(shell, "trap: action requires a signal specification");
            return 2;
        }
    }
    for (size_t i = operand; i < command->word_count; i++) {
        if (!ash_trap_is_exit(command->words[i])) {
            ash_diag(shell, "trap: unsupported signal specification '%s'", command->words[i]);
            return 1;
        }
    }
    if (print) {
        for (size_t i = operand; i < command->word_count; i++) {
            int status = ash_trap_print_exit(shell);
            if (status != 0) {
                return status;
            }
        }
        return 0;
    }
    if (!ash_trap_exit_set(shell, action)) {
        ash_diag_oom(shell);
        return 2;
    }
    return 0;
}
