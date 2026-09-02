#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/alias_builtins.h"
#include "applets/shell/ash/aliases.h"
#include "applets/shell/ash/command.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/shell_policy.h"

static bool ash_alias_write_quoted(const char* value) {
    if (fputc('\'', stdout) == EOF) {
        return false;
    }
    for (const char* cursor = value; *cursor != '\0'; cursor++) {
        if (*cursor == '\'') {
            if (fputs("'\\''", stdout) == EOF) {
                return false;
            }
        }
        else if (fputc((unsigned char)*cursor, stdout) == EOF) {
            return false;
        }
    }
    return fputc('\'', stdout) != EOF;
}

static bool ash_alias_print(
    const struct ash_alias* alias,
    bool prefix
) {
    return (!prefix || fputs("alias ", stdout) != EOF) &&
        fputs(ash_alias_name(alias), stdout) != EOF &&
        fputc('=', stdout) != EOF &&
        ash_alias_write_quoted(ash_alias_value(alias)) &&
        fputc('\n', stdout) != EOF;
}

static int ash_alias_print_all(
    struct ash_shell* shell,
    bool prefix
) {
    const struct ash_alias** aliases = NULL;
    size_t count = 0u;
    if (!ash_alias_snapshot(shell->aliases, &aliases, &count)) {
        ash_diag_oom(shell);
        return 2;
    }

    bool success = true;
    for (size_t i = 0u; i < count; i++) {
        if (!ash_alias_print(aliases[i], prefix)) {
            success = false;
            break;
        }
    }
    free(aliases);
    if (!success || fflush(stdout) == EOF) {
        ash_exec_error(
            shell,
            "alias",
            errno != 0 ? errno : EIO
        );
        return 1;
    }
    return 0;
}

static int ash_alias_invalid_option(
    struct ash_shell* shell,
    char option
) {
    ash_diag(shell, "alias: -%c: invalid option", option);
    ash_diag(shell, "alias: usage: alias [-p] [name[=value] ... ]");
    return 2;
}

static char ash_alias_parse_options(
    const struct ash_command* command,
    char accepted,
    size_t* operand,
    bool* enabled
) {
    *operand = 1u;
    *enabled = false;
    while (*operand < command->word_count &&
           command->words[*operand][0] == '-' &&
           command->words[*operand][1] != '\0') {
        const char* option = command->words[*operand];
        if (strcmp(option, "--") == 0) {
            (*operand)++;
            break;
        }
        for (size_t i = 1u; option[i] != '\0'; i++) {
            if (option[i] != accepted) {
                return option[i];
            }
            *enabled = true;
        }
        (*operand)++;
    }
    return '\0';
}

int ash_alias_builtin(
    struct ash_shell* shell,
    const struct ash_command* command
) {
    size_t operand;
    bool print_reusable;
    char invalid_option = ash_alias_parse_options(
        command,
        'p',
        &operand,
        &print_reusable
    );
    if (invalid_option != '\0') {
        return ash_alias_invalid_option(shell, invalid_option);
    }

    bool bash_output = ash_shell_policy_is_bash(&shell->policy);
    if (print_reusable || operand == command->word_count) {
        return ash_alias_print_all(
            shell,
            print_reusable || bash_output
        );
    }

    int status = 0;
    for (; operand < command->word_count; operand++) {
        const char* argument = command->words[operand];
        const char* equals = strchr(argument, '=');
        if (equals == NULL || equals == argument) {
            const struct ash_alias* alias = ash_alias_find(
                shell->aliases,
                argument
            );
            if (alias == NULL) {
                ash_diag(
                    shell,
                    "alias: %s: not found",
                    argument
                );
                status = 1;
            }
            else if (!ash_alias_print(alias, bash_output)) {
                ash_exec_error(
                    shell,
                    "alias",
                    errno != 0 ? errno : EIO
                );
                return 1;
            }
            continue;
        }

        size_t name_length = (size_t)(equals - argument);
        char* name = malloc(name_length + 1u);
        if (name == NULL) {
            ash_diag_oom(shell);
            return 2;
        }
        memcpy(name, argument, name_length);
        name[name_length] = '\0';
        if (!ash_alias_define(
                &shell->aliases,
                name,
                equals + 1
            )) {
            int error = errno;
            if (error == EINVAL) {
                ash_diag(
                    shell,
                    "alias: `%s': invalid alias name",
                    name
                );
                status = 1;
            }
            else {
                free(name);
                ash_diag_oom(shell);
                return 2;
            }
        }
        free(name);
    }
    if (fflush(stdout) == EOF) {
        ash_exec_error(
            shell,
            "alias",
            errno != 0 ? errno : EIO
        );
        return 1;
    }
    return status;
}

static int ash_unalias_invalid_option(
    struct ash_shell* shell,
    char option
) {
    ash_diag(shell, "unalias: -%c: invalid option", option);
    ash_diag(shell, "unalias: usage: unalias [-a] name [name ...]");
    return 2;
}

int ash_unalias_builtin(
    struct ash_shell* shell,
    const struct ash_command* command
) {
    size_t operand;
    bool remove_all;
    char invalid_option = ash_alias_parse_options(
        command,
        'a',
        &operand,
        &remove_all
    );
    if (invalid_option != '\0') {
        return ash_unalias_invalid_option(shell, invalid_option);
    }

    if (remove_all) {
        ash_aliases_destroy(&shell->aliases);
        return 0;
    }
    if (operand == command->word_count) {
        ash_diag(
            shell,
            "unalias: usage: unalias [-a] name [name ...]"
        );
        return 2;
    }

    int status = 0;
    for (; operand < command->word_count; operand++) {
        if (!ash_alias_unset(
                &shell->aliases,
                command->words[operand]
            )) {
            ash_diag(
                shell,
                "unalias: %s: not found",
                command->words[operand]
            );
            status = 1;
        }
    }
    return status;
}
