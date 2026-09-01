#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "applets/shell/ash/invocation.h"
#include "applets/shell/ash/shell_options.h"
#include "lib/cli_common.h"

static const char* ash_invocation_effective_name(const char* invoked) {
    const char* name = invoked;
    while (*name == '-') {
        name++;
    }
    return *name != '\0' ? name : "ash";
}

static bool ash_invocation_action_valid(
    enum ash_invocation_action action
) {
    return action >= ASH_INVOCATION_RUN &&
        action <= ASH_INVOCATION_VERSION;
}

bool ash_invocation_valid(const struct ash_invocation* invocation) {
    if (invocation == NULL ||
        invocation->invoked == NULL ||
        invocation->progname == NULL ||
        invocation->argv0 == NULL ||
        invocation->positional_count < 0 ||
        (invocation->positional_count > 0 &&
         invocation->positional_values == NULL) ||
        !ash_invocation_action_valid(invocation->action) ||
        !ash_shell_options_valid(invocation->options)) {
        return false;
    }
    for (int i = 0; i < invocation->positional_count; i++) {
        if (invocation->positional_values[i] == NULL) {
            return false;
        }
    }
    if (invocation->action != ASH_INVOCATION_RUN) {
        return invocation->input == ASH_STARTUP_STANDARD_INPUT &&
            invocation->command_string == NULL &&
            invocation->script_path == NULL;
    }

    switch (invocation->input) {
        case ASH_STARTUP_COMMAND_STRING:
            return invocation->command_string != NULL &&
                invocation->script_path == NULL;
        case ASH_STARTUP_SCRIPT_FILE:
            return invocation->command_string == NULL &&
                invocation->script_path != NULL;
        case ASH_STARTUP_STANDARD_INPUT:
            return invocation->command_string == NULL &&
                invocation->script_path == NULL;
    }
    return false;
}

bool ash_invocation_error_valid(
    const struct ash_invocation_error* error
) {
    if (error == NULL) {
        return false;
    }
    switch (error->kind) {
        case ASH_INVOCATION_ERROR_NONE:
            return error->progname == NULL &&
                error->argument == NULL &&
                error->sign == '\0' &&
                error->option == '\0';
        case ASH_INVOCATION_ERROR_INVALID_ARGUMENT:
            return error->progname == NULL &&
                error->argument == NULL &&
                error->sign == '\0' &&
                error->option == '\0';
        case ASH_INVOCATION_ERROR_INVALID_SHORT_OPTION:
            return error->progname != NULL &&
                error->argument == NULL &&
                (error->sign == '-' || error->sign == '+') &&
                error->option != '\0';
        case ASH_INVOCATION_ERROR_INVALID_LONG_OPTION:
        case ASH_INVOCATION_ERROR_INVALID_OPTION_NAME:
            return error->progname != NULL &&
                error->argument != NULL &&
                error->sign == '\0' &&
                error->option == '\0';
        case ASH_INVOCATION_ERROR_MISSING_COMMAND:
            return error->progname != NULL &&
                error->argument == NULL &&
                error->sign == '\0' &&
                error->option == '\0';
    }
    return false;
}

static bool ash_invocation_fail(
    struct ash_invocation_error* output,
    struct ash_invocation_error error
) {
    if (output != NULL) {
        *output = error;
    }
    return false;
}

static bool ash_invocation_apply_short(
    struct ash_invocation* candidate,
    char sign,
    char option,
    bool* command_mode,
    struct ash_invocation_error* error
) {
    bool enabled = sign == '-';
    switch (option) {
        case 'c':
            *command_mode = true;
            return true;
        case 'i':
            candidate->force_interactive = enabled;
            return true;
    }
    if (ash_shell_option_apply_letter(
            &candidate->options,
            option,
            enabled,
            ASH_SHELL_OPTION_USE_INVOCATION_SHORT
        ) == ASH_SHELL_OPTION_APPLIED) {
        return true;
    }

    return ash_invocation_fail(
        error,
        (struct ash_invocation_error){
            .kind = ASH_INVOCATION_ERROR_INVALID_SHORT_OPTION,
            .progname = candidate->progname,
            .sign = sign,
            .option = option,
        }
    );
}

static bool ash_invocation_apply_long(
    struct ash_invocation* candidate,
    const char* argument,
    struct ash_invocation_error* error
) {
    if (strcmp(argument, "--help") == 0) {
        candidate->action = ASH_INVOCATION_HELP;
        return true;
    }
    if (strcmp(argument, "--version") == 0) {
        candidate->action = ASH_INVOCATION_VERSION;
        return true;
    }
    if (strcmp(argument, "--standalone-applets") == 0) {
        candidate->standalone_applets = true;
        return true;
    }
    if (strcmp(argument, "--verbose") == 0 &&
        strcmp(candidate->progname, "bash") == 0 &&
        ash_shell_option_apply_name(
            &candidate->options,
            "verbose",
            true,
            ASH_SHELL_OPTION_USE_INVOCATION_NAME
        ) == ASH_SHELL_OPTION_APPLIED) {
        return true;
    }

    return ash_invocation_fail(
        error,
        (struct ash_invocation_error){
            .kind = ASH_INVOCATION_ERROR_INVALID_LONG_OPTION,
            .progname = candidate->progname,
            .argument = argument,
        }
    );
}

bool ash_invocation_parse(
    int argc,
    char** argv,
    struct ash_invocation* invocation,
    struct ash_invocation_error* error
) {
    if (argc <= 0 || argv == NULL || invocation == NULL ||
        error == NULL || argv[0] == NULL) {
        return ash_invocation_fail(
            error,
            (struct ash_invocation_error){
                .kind = ASH_INVOCATION_ERROR_INVALID_ARGUMENT,
            }
        );
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i] == NULL) {
            return ash_invocation_fail(
                error,
                (struct ash_invocation_error){
                    .kind = ASH_INVOCATION_ERROR_INVALID_ARGUMENT,
                }
            );
        }
    }

    const char* raw_argv0 = argv[0];
    const char* invoked = bx_cli_progname(raw_argv0, "ash");
    struct ash_invocation candidate = {
        .invoked = invoked,
        .progname = ash_invocation_effective_name(invoked),
        .argv0 = invoked,
        .action = ASH_INVOCATION_RUN,
        .input = ASH_STARTUP_STANDARD_INPUT,
        .login_shell = invoked[0] == '-',
    };
    struct ash_invocation_error parse_error = {0};
    bool command_mode = false;
    bool long_options_allowed = true;
    int index = 1;

    while (index < argc) {
        const char* argument = argv[index];
        if (strcmp(argument, "--") == 0 ||
            strcmp(argument, "-") == 0) {
            index++;
            break;
        }
        if ((argument[0] != '-' && argument[0] != '+') ||
            argument[1] == '\0') {
            break;
        }

        if (argument[0] == '-' && argument[1] == '-' &&
            long_options_allowed) {
            if (!ash_invocation_apply_long(
                    &candidate,
                    argument,
                    &parse_error
                )) {
                return ash_invocation_fail(error, parse_error);
            }
            index++;
            if (candidate.action != ASH_INVOCATION_RUN) {
                candidate.positional_values = argv + index;
                candidate.positional_count = argc - index;
                if (!ash_invocation_valid(&candidate)) {
                    return ash_invocation_fail(
                        error,
                        (struct ash_invocation_error){
                            .kind =
                                ASH_INVOCATION_ERROR_INVALID_ARGUMENT,
                        }
                    );
                }
                *invocation = candidate;
                *error = (struct ash_invocation_error){0};
                return true;
            }
            continue;
        }

        long_options_allowed = false;
        char sign = argument[0];
        for (const char* option = argument + 1;
             *option != '\0';
             option++) {
            if (*option == 'o' &&
                strcmp(candidate.progname, "bash") == 0 &&
                index + 1 < argc) {
                const char* option_name = argv[++index];
                if (ash_shell_option_apply_name(
                        &candidate.options,
                        option_name,
                        sign == '-',
                        ASH_SHELL_OPTION_USE_INVOCATION_NAME
                    ) != ASH_SHELL_OPTION_APPLIED) {
                    return ash_invocation_fail(
                        error,
                        (struct ash_invocation_error){
                            .kind =
                                ASH_INVOCATION_ERROR_INVALID_OPTION_NAME,
                            .progname = candidate.progname,
                            .argument = option_name,
                        }
                    );
                }
                continue;
            }
            if (!ash_invocation_apply_short(
                    &candidate,
                    sign,
                    *option,
                    &command_mode,
                    &parse_error
                )) {
                return ash_invocation_fail(error, parse_error);
            }
        }
        index++;
    }

    bool read_stdin =
        (candidate.options & ASH_SHELL_OPTION_STDIN) != 0u;
    if (command_mode) {
        if (index >= argc) {
            return ash_invocation_fail(
                error,
                (struct ash_invocation_error){
                    .kind = ASH_INVOCATION_ERROR_MISSING_COMMAND,
                    .progname = candidate.progname,
                }
            );
        }
        candidate.input = ASH_STARTUP_COMMAND_STRING;
        candidate.command_string = argv[index++];
        if (index < argc) {
            candidate.argv0 = argv[index++];
        }
    }
    else if (!read_stdin && index < argc) {
        candidate.input = ASH_STARTUP_SCRIPT_FILE;
        candidate.script_path = argv[index++];
        candidate.argv0 = candidate.script_path;
    }
    candidate.positional_values = argv + index;
    candidate.positional_count = argc - index;

    if (!ash_invocation_valid(&candidate)) {
        return ash_invocation_fail(
            error,
            (struct ash_invocation_error){
                .kind = ASH_INVOCATION_ERROR_INVALID_ARGUMENT,
            }
        );
    }
    *invocation = candidate;
    *error = (struct ash_invocation_error){0};
    return true;
}
