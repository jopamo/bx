#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "applets/shell/ash/command.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/trap_builtin.h"
#include "applets/shell/ash/traps.h"
#include "lib/output_quote.h"
#include "lib/signal_names.h"

static bool ash_trap_is_exit(const char* specification) {
    return strcmp(specification, "EXIT") == 0 || (specification[0] != '\0' && strspn(specification, "0") == strlen(specification));
}

static const char* const ash_trap_pseudo_names[] = {"DEBUG", "ERR", "RETURN"};

static bool ash_trap_specification(const char* specification, int* number) {
    if (ash_trap_is_exit(specification)) {
        *number = 0;
        return true;
    }
    for (size_t i = 0; i < sizeof(ash_trap_pseudo_names) / sizeof(ash_trap_pseudo_names[0]); i++) {
        if (strcmp(specification, ash_trap_pseudo_names[i]) == 0) {
            *number = ash_trap_signal_limit() + (int)i;
            return true;
        }
    }
    return bx_signal_name_lookup(specification, number) && *number > 0 && *number < ash_trap_signal_limit();
}

static int ash_trap_print(struct ash_shell* shell, int number, bool defaults) {
    const char* action = number == 0 ? ash_trap_exit_action(shell) : ash_trap_signal_action(shell, number);
    if (action == NULL && !defaults) {
        return 0;
    }
    char name[32];
    const char* prefix = "";
    if (number == 0) {
        strcpy(name, "EXIT");
    }
    else if (number >= ash_trap_signal_limit()) {
        snprintf(name, sizeof(name), "%s", ash_trap_pseudo_names[number - ash_trap_signal_limit()]);
    }
#ifdef SIGIO
    else if (number == SIGIO) {
        prefix = "SIG";
        strcpy(name, "IO");
    }
#endif
    else if (bx_signal_name_format(number, name, sizeof(name))) {
        prefix = "SIG";
    }
    else {
        snprintf(name, sizeof(name), "%d", number);
    }
    if (ash_shell_policy_has(&shell->policy, ASH_SHELL_POLICY_POSIX)) {
        prefix = "";
    }
    if (fputs("trap -- ", stdout) == EOF || (action == NULL ? fputs("-", stdout) == EOF : !bx_output_quote_write_single(stdout, action)) || fprintf(stdout, " %s%s\n", prefix, name) < 0 ||
        fflush(stdout) == EOF) {
        ash_exec_error(shell, "trap", errno != 0 ? errno : EIO);
        return 1;
    }
    return 0;
}

static int ash_trap_error(struct ash_shell* shell, int error) {
    if (error == ENOMEM) {
        ash_diag_oom(shell);
        return 2;
    }
    ash_exec_error(shell, "trap", error);
    return 1;
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
    int error = ash_trap_signals_init(shell);
    if (error != 0) {
        return ash_trap_error(shell, error);
    }
    if (operand == command->word_count) {
        bool defaults = print && ash_shell_policy_has(&shell->policy, ASH_SHELL_POLICY_POSIX);
        for (int number = 0; number < ash_trap_signal_limit() + (int)(sizeof(ash_trap_pseudo_names) / sizeof(ash_trap_pseudo_names[0])); number++) {
            int status = ash_trap_print(shell, number, defaults);
            if (status != 0) {
                return status;
            }
        }
        return 0;
    }
    const char* action = NULL;
    int first_number;
    const char* first = command->words[operand];
    bool numeric = first[0] != '\0' && strspn(first, "0123456789") == strlen(first);
    bool shorthand = ash_trap_specification(first, &first_number) && (numeric || (operand + 1u == command->word_count && !ash_shell_policy_has(&shell->policy, ASH_SHELL_POLICY_POSIX)));
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
    bool exit_selected = false;
    for (size_t i = operand; i < command->word_count; i++) {
        int number;
        if (!ash_trap_specification(command->words[i], &number) || (!print && number != 0 && !ash_trap_signal_supported(shell, number, action))) {
            ash_diag(shell, "trap: unsupported signal specification '%s'", command->words[i]);
            return 1;
        }
        exit_selected = exit_selected || number == 0;
    }
    if (print) {
        for (size_t i = operand; i < command->word_count; i++) {
            int number;
            (void)ash_trap_specification(command->words[i], &number);
            int status = ash_trap_print(shell, number, ash_shell_policy_has(&shell->policy, ASH_SHELL_POLICY_POSIX));
            if (status != 0) {
                return status;
            }
        }
        return 0;
    }
    if (exit_selected && !ash_trap_exit_set(shell, action)) {
        ash_diag_oom(shell);
        return 2;
    }
    for (size_t i = operand; i < command->word_count; i++) {
        int number;
        (void)ash_trap_specification(command->words[i], &number);
        if (number != 0) {
            error = ash_trap_signal_set(shell, number, action);
            if (error != 0) {
                return ash_trap_error(shell, error);
            }
        }
    }
    return 0;
}
