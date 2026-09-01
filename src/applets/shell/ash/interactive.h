#ifndef BX_APPLETS_SHELL_ASH_INTERACTIVE_H
#define BX_APPLETS_SHELL_ASH_INTERACTIVE_H

#include <stdbool.h>
#include <stdint.h>

enum ash_startup_input {
    ASH_STARTUP_COMMAND_STRING = 0,
    ASH_STARTUP_SCRIPT_FILE,
    ASH_STARTUP_STANDARD_INPUT,
};

enum ash_interactive_mode {
    ASH_INTERACTIVE_DISABLED = 0,
    ASH_INTERACTIVE_IMPLICIT,
    ASH_INTERACTIVE_FORCED,
};

enum ash_terminal_attachment {
    ASH_TERMINAL_DETACHED = 0u,
    ASH_TERMINAL_STANDARD_INPUT = 1u << 0,
    ASH_TERMINAL_DIAGNOSTIC = 1u << 1,
};

/*
 * This is startup truth about terminal descriptors, not authority to perform
 * job-control ioctls. Controlling-terminal ownership requires a separate
 * validated lifecycle.
 */
struct ash_interactive_state {
    enum ash_startup_input input;
    enum ash_interactive_mode mode;
    uint32_t terminal_attachments;
};

bool ash_interactive_state_resolve(
    enum ash_startup_input input,
    bool force_interactive,
    bool standard_input_terminal,
    bool diagnostic_terminal,
    struct ash_interactive_state* state
);
bool ash_interactive_state_valid(
    const struct ash_interactive_state* state
);
bool ash_interactive_state_enabled(
    const struct ash_interactive_state* state
);
bool ash_interactive_state_should_prompt(
    const struct ash_interactive_state* state,
    bool active_script_terminal
);
bool ash_terminal_fd_attached(int fd);

#endif /* BX_APPLETS_SHELL_ASH_INTERACTIVE_H */
