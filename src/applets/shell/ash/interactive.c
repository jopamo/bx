#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "applets/shell/ash/interactive.h"

static bool ash_startup_input_valid(enum ash_startup_input input) {
    return input >= ASH_STARTUP_COMMAND_STRING &&
        input <= ASH_STARTUP_STANDARD_INPUT;
}

bool ash_interactive_state_valid(
    const struct ash_interactive_state* state
) {
    const uint32_t known_attachments =
        ASH_TERMINAL_STANDARD_INPUT |
        ASH_TERMINAL_DIAGNOSTIC;
    if (state == NULL ||
        !ash_startup_input_valid(state->input) ||
        (state->terminal_attachments & ~known_attachments) != 0u) {
        return false;
    }

    bool implicit_candidate =
        state->input == ASH_STARTUP_STANDARD_INPUT &&
        (state->terminal_attachments & known_attachments) ==
            known_attachments;
    switch (state->mode) {
        case ASH_INTERACTIVE_DISABLED:
            return !implicit_candidate;
        case ASH_INTERACTIVE_IMPLICIT:
            return implicit_candidate;
        case ASH_INTERACTIVE_FORCED:
            return true;
    }
    return false;
}

bool ash_interactive_state_resolve(
    enum ash_startup_input input,
    bool force_interactive,
    bool standard_input_terminal,
    bool diagnostic_terminal,
    struct ash_interactive_state* state
) {
    if (!ash_startup_input_valid(input) || state == NULL) {
        return false;
    }

    struct ash_interactive_state candidate = {
        .input = input,
        .terminal_attachments =
            (standard_input_terminal ?
                ASH_TERMINAL_STANDARD_INPUT :
                ASH_TERMINAL_DETACHED) |
            (diagnostic_terminal ?
                ASH_TERMINAL_DIAGNOSTIC :
                ASH_TERMINAL_DETACHED),
    };
    bool implicit = input == ASH_STARTUP_STANDARD_INPUT &&
        standard_input_terminal &&
        diagnostic_terminal;
    candidate.mode = force_interactive ?
        ASH_INTERACTIVE_FORCED :
        (implicit ?
            ASH_INTERACTIVE_IMPLICIT :
            ASH_INTERACTIVE_DISABLED);
    if (!ash_interactive_state_valid(&candidate)) {
        return false;
    }
    *state = candidate;
    return true;
}

bool ash_interactive_state_enabled(
    const struct ash_interactive_state* state
) {
    return ash_interactive_state_valid(state) &&
        state->mode != ASH_INTERACTIVE_DISABLED;
}

bool ash_interactive_state_should_prompt(
    const struct ash_interactive_state* state,
    bool active_script_terminal
) {
    if (!ash_interactive_state_enabled(state)) {
        return false;
    }
    switch (state->input) {
        case ASH_STARTUP_COMMAND_STRING:
            return false;
        case ASH_STARTUP_SCRIPT_FILE:
            return active_script_terminal;
        case ASH_STARTUP_STANDARD_INPUT:
            return true;
    }
    return false;
}

bool ash_terminal_fd_attached(int fd) {
    int saved_errno = errno;
    bool attached = fd >= 0 && isatty(fd) == 1;
    errno = saved_errno;
    return attached;
}
