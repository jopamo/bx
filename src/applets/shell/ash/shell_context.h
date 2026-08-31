#ifndef BX_APPLETS_SHELL_ASH_SHELL_CONTEXT_H
#define BX_APPLETS_SHELL_ASH_SHELL_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "applets/shell/ash/control.h"

struct ash_alias;
struct ash_command_cache;
struct ash_function;
struct ash_input_source;
struct ash_job;
struct ash_trap_table;
struct ash_var;
struct ash_shell;

typedef bool (*ash_command_substitution_fn)(
    struct ash_shell* shell,
    const char* command,
    size_t length,
    char** output
);

enum ash_shell_option {
    ASH_SHELL_OPTION_ALLEXPORT = 1u << 0,
    ASH_SHELL_OPTION_NOTIFY = 1u << 1,
    ASH_SHELL_OPTION_NOCLOBBER = 1u << 2,
    ASH_SHELL_OPTION_ERREXIT = 1u << 3,
    ASH_SHELL_OPTION_NOGLOB = 1u << 4,
    ASH_SHELL_OPTION_MONITOR = 1u << 5,
    ASH_SHELL_OPTION_NOEXEC = 1u << 6,
    ASH_SHELL_OPTION_NOUNSET = 1u << 7,
    ASH_SHELL_OPTION_VERBOSE = 1u << 8,
    ASH_SHELL_OPTION_XTRACE = 1u << 9,
    ASH_SHELL_OPTION_INTERACTIVE = 1u << 10,
    ASH_SHELL_OPTION_STDIN = 1u << 11,
};

struct ash_positional_frame {
    const char* argv0;
    char** values;
    int count;
    struct ash_positional_frame* previous;
};

struct ash_cwd_state {
    char* physical;
    char* logical;
    char* old_logical;
};

/*
 * One invocation owns one context. These pointers are the sole roots for
 * shell-language state; subsystems may not publish parallel global authority.
 */
struct ash_shell {
    const char* progname;
    struct ash_positional_frame positionals;
    struct ash_var* vars;
    uint32_t options;

    struct ash_alias* aliases;
    struct ash_function* functions;
    struct ash_trap_table* traps;
    struct ash_job* jobs;
    struct ash_command_cache* command_cache;
    struct ash_input_source* input_stack;
    struct ash_cwd_state cwd;

    pid_t shell_pid;
    pid_t last_async_pid;
    int last_status;
    bool interactive;
    bool login_shell;
    bool should_exit;
    int requested_exit_status;
    struct ash_control_state control;
    ash_command_substitution_fn command_substitution;
};

void ash_shell_option_letters(const struct ash_shell* shell, char* output, size_t output_size);
void ash_shell_context_release_owned(struct ash_shell* shell);

#endif /* BX_APPLETS_SHELL_ASH_SHELL_CONTEXT_H */
