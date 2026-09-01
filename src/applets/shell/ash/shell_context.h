#ifndef BX_APPLETS_SHELL_ASH_SHELL_CONTEXT_H
#define BX_APPLETS_SHELL_ASH_SHELL_CONTEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "applets/shell/ash/control.h"
#include "applets/shell/ash/parser.h"
#include "applets/shell/ash/scope.h"
#include "applets/shell/ash/shell_policy.h"
#include "lib/fd_transaction.h"

struct ash_alias;
struct ash_command_cache;
struct ash_completion_state;
struct ash_function;
struct ash_history_state;
struct ash_input_source;
struct ash_job;
struct ash_shopt_state;
struct ash_source_name;
struct ash_trap_table;
struct ash_shell;

typedef bool (*ash_command_substitution_fn)(
    struct ash_shell* shell,
    const char* command,
    size_t length,
    char** output
);
/*
 * The command span is borrowed. On success output receives one caller-owned
 * allocation; on failure output remains NULL.
 */

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
    ASH_SHELL_OPTION_STDIN = 1u << 10,
    ASH_SHELL_OPTION_ALL = (1u << 11) - 1u,
};

struct ash_cwd_state {
    char* physical;
    char* logical;
    char* old_logical;
};

struct ash_parser_state {
    struct ash_parser parser;
    bool active;
};

struct ash_shell_context_config {
    const char* progname;
    const char* argv0;
    char** positional_values;
    int positional_count;
    uint32_t options;
    struct ash_shell_policy policy;
    pid_t shell_pid;
    ash_command_substitution_fn command_substitution;
};

/*
 * Shell ownership contract:
 *
 * - const pointer parameters are borrowed for the call unless an API states a
 *   longer lifetime.
 * - init/destroy pairs own all nested allocations between those calls.
 * - clone APIs deep-copy owned data.
 * - take APIs transfer ownership only on success and clear the source.
 * - returned mutable strings and output collections belong to the caller.
 * - source identities referenced by tokens, words, ASTs, and functions live
 *   for the shell-context lifetime.
 * - descriptor transactions own only their saved backup descriptors; restore
 *   rolls back target descriptors and commit keeps target changes.
 *
 * One invocation owns one active context. These fields are the sole roots for
 * shell-language state; subsystems may not publish parallel global authority.
 * Invocation strings and positional arrays are borrowed for the context
 * lifetime. Subsystem root pointers are context-owned when non-NULL. Every
 * stable execution boundary satisfies ash_shell_context_invariants(); a
 * subsystem may violate only its own invariant while a private transition is
 * in progress and must restore it before returning.
 */
struct ash_shell {
    const char* progname;
    /*
     * The scope stack is the sole root for variables and positional
     * parameters. Its bottom frame is always the global scope.
     */
    struct ash_scope* scopes;
    uint32_t options;
    struct ash_shopt_state* shopt;
    struct ash_shell_policy policy;

    struct ash_alias* aliases;
    struct ash_function* functions;
    struct ash_trap_table* traps;
    /* The jobs root is authoritative for all child lifecycle records. */
    struct ash_job* jobs;
    unsigned long next_job_id;
    struct ash_command_cache* command_cache;
    struct ash_input_source* input_stack;
    struct ash_source_name* source_names;
    struct ash_parser_state parser_state;
    struct ash_history_state* history;
    struct ash_completion_state* completion;
    struct ash_cwd_state cwd;
    /* Sole owner of current-shell redirection backup descriptors. */
    struct bx_fd_transaction_stack redirections;

    pid_t shell_pid;
    /* Cached from the latest committed published async job for `$!`. */
    pid_t last_async_pid;
    int last_status;
    bool should_exit;
    int requested_exit_status;
    struct ash_control_state control;
    ash_command_substitution_fn command_substitution;
};

bool ash_shell_context_init(
    struct ash_shell* shell,
    const struct ash_shell_context_config* config
);
bool ash_shell_context_invariants(const struct ash_shell* shell);
void ash_shell_context_assert_invariants(const struct ash_shell* shell);
struct ash_parser* ash_shell_context_begin_parse(
    struct ash_shell* shell,
    const char* source_name,
    const char* input,
    size_t length
);
void ash_shell_context_end_parse(struct ash_shell* shell);
/*
 * A fork child keeps language state but must withdraw inherited parent-only
 * child records and redirection backups before executing shell semantics.
 */
void ash_shell_context_detach_after_fork(struct ash_shell* shell);
void ash_shell_option_letters(const struct ash_shell* shell, char* output, size_t output_size);
void ash_shell_context_release_owned(struct ash_shell* shell);

#endif /* BX_APPLETS_SHELL_ASH_SHELL_CONTEXT_H */
