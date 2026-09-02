#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "applets/shell/ash/command.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/shopt.h"
#include "applets/shell/ash/shopt_builtin.h"
#include "bx/diag.h"

enum ash_shopt_output {
    ASH_SHOPT_OUTPUT_STATUS = 0,
    ASH_SHOPT_OUTPUT_REUSABLE,
    ASH_SHOPT_OUTPUT_QUIET,
};

static int ash_shopt_usage(struct ash_shell* shell) {
    ash_diag(
        shell,
        "shopt: usage: shopt [-pqsu] [optname ...]"
    );
    return 2;
}

static bool ash_shopt_write(
    enum ash_shopt_option option,
    bool enabled,
    enum ash_shopt_output output
) {
    const char* name = ash_shopt_name(option);
    if (name == NULL) {
        return false;
    }
    errno = 0;
    switch (output) {
        case ASH_SHOPT_OUTPUT_STATUS:
            return printf(
                "%-20s\t%s\n",
                name,
                enabled ? "on" : "off"
            ) >= 0;
        case ASH_SHOPT_OUTPUT_REUSABLE:
            return printf(
                "shopt -%c %s\n",
                enabled ? 's' : 'u',
                name
            ) >= 0;
        case ASH_SHOPT_OUTPUT_QUIET:
            return true;
    }
    return false;
}

static int ash_shopt_flush(struct ash_shell* shell) {
    int output_error = errno;
    if (fflush(stdout) == EOF && errno != 0) {
        output_error = errno;
    }
    if (ferror(stdout)) {
        clearerr(stdout);
        ash_diag(
            shell,
            "shopt: write error: %s",
            bx_strerror(output_error != 0 ? output_error : EIO)
        );
        return 1;
    }
    return 0;
}

int ash_shopt_builtin(
    struct ash_shell* shell,
    const struct ash_command* command
) {
    bool set = false;
    bool unset = false;
    enum ash_shopt_output output = ASH_SHOPT_OUTPUT_STATUS;
    size_t operand = 1u;
    while (operand < command->word_count &&
           command->words[operand][0] == '-' &&
           command->words[operand][1] != '\0') {
        const char* argument = command->words[operand];
        if (strcmp(argument, "--") == 0) {
            operand++;
            break;
        }
        for (size_t i = 1u; argument[i] != '\0'; i++) {
            switch (argument[i]) {
                case 'p':
                    output = ASH_SHOPT_OUTPUT_REUSABLE;
                    break;
                case 'q':
                    output = ASH_SHOPT_OUTPUT_QUIET;
                    break;
                case 's':
                    set = true;
                    break;
                case 'u':
                    unset = true;
                    break;
                default:
                    return ash_shopt_usage(shell);
            }
        }
        operand++;
    }
    if (set && unset) {
        ash_diag(
            shell,
            "shopt: cannot set and unset shell options "
            "simultaneously"
        );
        return 1;
    }

    bool named = operand != command->word_count;
    if (!named) {
        bool enabled = ash_shopt_enabled(
            &shell->shopt,
            ASH_SHOPT_EXTGLOB
        );
        if (set || unset) {
            if ((set && enabled) || (unset && !enabled)) {
                if (!ash_shopt_write(
                        ASH_SHOPT_EXTGLOB,
                        enabled,
                        ASH_SHOPT_OUTPUT_STATUS
                    )) {
                    return ash_shopt_flush(shell);
                }
            }
        }
        else if (!ash_shopt_write(ASH_SHOPT_EXTGLOB, enabled, output)) {
            return ash_shopt_flush(shell);
        }
        return ash_shopt_flush(shell);
    }

    int status = 0;
    while (operand < command->word_count) {
        const char* name = command->words[operand++];
        enum ash_shopt_option option = ash_shopt_resolve(name);
        if (option == 0u) {
            ash_diag(
                shell,
                "shopt: %s: invalid shell option name",
                name
            );
            status = 1;
            continue;
        }

        if (set || unset) {
            enum ash_shopt_result result = ash_shopt_apply(
                &shell->shopt,
                option,
                set,
                shell->policy.personality
            );
            if (result != ASH_SHOPT_APPLIED) {
                ash_diag(
                    shell,
                    "shopt: %s: invalid shell option name",
                    name
                );
                status = 1;
            }
            continue;
        }

        bool enabled = ash_shopt_enabled(&shell->shopt, option);
        if (!ash_shopt_write(option, enabled, output)) {
            return ash_shopt_flush(shell);
        }
        if (!enabled) {
            status = 1;
        }
    }

    int write_status = ash_shopt_flush(shell);
    return write_status != 0 ? write_status : status;
}
