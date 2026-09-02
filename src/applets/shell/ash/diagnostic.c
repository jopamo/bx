#include <stdarg.h>
#include <stdio.h>

#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/shell_context.h"
#include "bx/diag.h"

enum ash_diagnostic_phase {
    ASH_DIAGNOSTIC_RUNTIME = 0,
    ASH_DIAGNOSTIC_PARSE,
};

static bool ash_diag_location_active(
    const struct ash_source_location* location
) {
    return ash_source_location_valid(location) &&
        location->identity != NULL &&
        location->identity->published &&
        location->identity->reference_count != 0u &&
        location->source == location->identity->name;
}

static bool ash_diag_synthetic_source(enum ash_input_kind kind) {
    switch (kind) {
        case ASH_INPUT_EVAL:
        case ASH_INPUT_COMMAND_SUBSTITUTION:
        case ASH_INPUT_PROMPT_COMMAND:
        case ASH_INPUT_COMPLETION_HOOK:
            return true;
        case ASH_INPUT_INVALID:
        case ASH_INPUT_COMMAND_STRING:
        case ASH_INPUT_STDIN:
        case ASH_INPUT_SCRIPT_FILE:
        case ASH_INPUT_SOURCED_FILE:
        case ASH_INPUT_INTERACTIVE:
            return false;
    }
    return false;
}

static struct ash_source_location ash_diag_runtime_location(
    struct ash_source_location location
) {
    while (ash_diag_location_active(&location) &&
           ash_diag_synthetic_source(location.identity->kind) &&
           ash_diag_location_active(&location.identity->caller)) {
        location = location.identity->caller;
    }
    return location;
}

static const char* ash_diag_source_name(
    const struct ash_shell* shell,
    const struct ash_source_location* location
) {
    if (!ash_diag_location_active(location)) {
        return shell->progname;
    }
    switch (location->identity->kind) {
        case ASH_INPUT_SCRIPT_FILE:
        case ASH_INPUT_SOURCED_FILE:
            return location->source;
        case ASH_INPUT_INVALID:
        case ASH_INPUT_COMMAND_STRING:
        case ASH_INPUT_STDIN:
        case ASH_INPUT_EVAL:
        case ASH_INPUT_COMMAND_SUBSTITUTION:
        case ASH_INPUT_PROMPT_COMMAND:
        case ASH_INPUT_COMPLETION_HOOK:
        case ASH_INPUT_INTERACTIVE:
            return shell->progname;
    }
    return shell->progname;
}

static void ash_diag_prefix(
    const struct ash_shell* shell,
    enum ash_diagnostic_phase phase,
    struct ash_source_location location
) {
    if (!ash_diag_location_active(&location)) {
        fprintf(stderr, "%s: ", shell->progname);
        return;
    }

    if (phase == ASH_DIAGNOSTIC_RUNTIME) {
        location = ash_diag_runtime_location(location);
        fprintf(
            stderr,
            "%s: line %zu: ",
            ash_diag_source_name(shell, &location),
            location.line
        );
        return;
    }

    const char* qualifier = NULL;
    struct ash_source_location source_location = location;
    switch (location.identity->kind) {
        case ASH_INPUT_COMMAND_STRING:
            qualifier = "-c";
            break;
        case ASH_INPUT_EVAL:
            qualifier = "eval";
            source_location = ash_diag_runtime_location(
                location.identity->caller
            );
            break;
        case ASH_INPUT_COMMAND_SUBSTITUTION:
            qualifier = "command substitution";
            source_location = ash_diag_runtime_location(
                location.identity->caller
            );
            break;
        case ASH_INPUT_PROMPT_COMMAND:
            qualifier = "PROMPT_COMMAND";
            source_location = ash_diag_runtime_location(
                location.identity->caller
            );
            break;
        case ASH_INPUT_COMPLETION_HOOK:
            qualifier = "completion";
            source_location = ash_diag_runtime_location(
                location.identity->caller
            );
            break;
        case ASH_INPUT_INVALID:
        case ASH_INPUT_STDIN:
        case ASH_INPUT_SCRIPT_FILE:
        case ASH_INPUT_SOURCED_FILE:
        case ASH_INPUT_INTERACTIVE:
            break;
    }
    fprintf(
        stderr,
        "%s: ",
        ash_diag_source_name(shell, &source_location)
    );
    if (qualifier != NULL) {
        fprintf(stderr, "%s: ", qualifier);
    }
    fprintf(stderr, "line %zu: ", location.line);
}

static void ash_diag_v(
    const struct ash_shell* shell,
    enum ash_diagnostic_phase phase,
    struct ash_source_location location,
    const char* format,
    va_list arguments
) {
    ash_diag_prefix(shell, phase, location);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
}

void ash_diag(const struct ash_shell* shell, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    ash_diag_v(
        shell,
        ASH_DIAGNOSTIC_RUNTIME,
        shell->execution_location,
        format,
        arguments
    );
    va_end(arguments);
}

void ash_diag_parse(
    const struct ash_shell* shell,
    struct ash_source_location location,
    const char* format,
    ...
) {
    va_list arguments;
    va_start(arguments, format);
    ash_diag_v(
        shell,
        ASH_DIAGNOSTIC_PARSE,
        location,
        format,
        arguments
    );
    va_end(arguments);
}

bool ash_diag_oom(const struct ash_shell* shell) {
    ash_diag(shell, "out of memory");
    return false;
}

void ash_exec_error(
    const struct ash_shell* shell,
    const char* subject,
    int error
) {
    ash_diag(shell, "%s: %s", subject, bx_strerror(error));
}

void ash_exec_not_found(
    const struct ash_shell* shell,
    const char* command
) {
    ash_diag(shell, "%s: not found", command);
}
