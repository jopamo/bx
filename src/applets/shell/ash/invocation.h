#ifndef BX_APPLETS_SHELL_ASH_INVOCATION_H
#define BX_APPLETS_SHELL_ASH_INVOCATION_H

#include <stdbool.h>
#include <stdint.h>

#include "applets/shell/ash/interactive.h"

enum ash_invocation_action {
    ASH_INVOCATION_RUN = 0,
    ASH_INVOCATION_HELP,
    ASH_INVOCATION_VERSION,
};

enum ash_invocation_error_kind {
    ASH_INVOCATION_ERROR_NONE = 0,
    ASH_INVOCATION_ERROR_INVALID_ARGUMENT,
    ASH_INVOCATION_ERROR_INVALID_SHORT_OPTION,
    ASH_INVOCATION_ERROR_INVALID_LONG_OPTION,
    ASH_INVOCATION_ERROR_INVALID_OPTION_NAME,
    ASH_INVOCATION_ERROR_MISSING_COMMAND,
};

/*
 * Every pointer borrows argv storage for the invocation lifetime. Parsing
 * commits this normalized request only after the complete argv boundary is
 * valid.
 */
struct ash_invocation {
    const char* invoked;
    const char* progname;
    const char* argv0;
    char** positional_values;
    int positional_count;
    enum ash_invocation_action action;
    enum ash_startup_input input;
    const char* command_string;
    const char* script_path;
    uint32_t options;
    bool login_shell;
    bool force_interactive;
    bool standalone_applets;
};

struct ash_invocation_error {
    enum ash_invocation_error_kind kind;
    const char* progname;
    const char* argument;
    char sign;
    char option;
};

bool ash_invocation_parse(
    int argc,
    char** argv,
    struct ash_invocation* invocation,
    struct ash_invocation_error* error
);
bool ash_invocation_valid(const struct ash_invocation* invocation);
bool ash_invocation_error_valid(
    const struct ash_invocation_error* error
);

#endif /* BX_APPLETS_SHELL_ASH_INVOCATION_H */
