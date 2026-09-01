#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets/shell/ash/input.h"
#include "applets/shell/ash/functions.h"
#include "applets/shell/ash/process.h"
#include "applets/shell/ash/scope.h"
#include "applets/shell/ash/shell_context.h"
#include "lib/fd_transaction.h"

static bool ash_parser_state_invariants(const struct ash_shell* shell) {
    const struct ash_parser_state* state = &shell->parser_state;
    if (!state->active) {
        return state->parser.lexer.source_name == NULL &&
            state->parser.lexer.source_identity == NULL &&
            state->parser.lexer.input == NULL &&
            !state->parser.has_lookahead &&
            state->parser.lookahead.word.parts == NULL &&
            state->parser.lookahead.io_number == NULL &&
            state->parser.error == NULL;
    }

    const struct ash_lexer* lexer = &state->parser.lexer;
    if (lexer->source_name == NULL || lexer->input == NULL ||
        lexer->offset > lexer->length ||
        lexer->source_offset > SIZE_MAX - lexer->length ||
        lexer->line == 0u || lexer->column == 0u ||
        state->parser.result < ASH_PARSER_COMPLETE ||
        state->parser.result > ASH_PARSER_ERROR ||
        (state->parser.has_lookahead &&
         (state->parser.lookahead.kind < ASH_TOKEN_EOF ||
          state->parser.lookahead.kind > ASH_TOKEN_CLOBBER))) {
        return false;
    }
    return shell->input_stack == NULL ||
        (lexer->source_name == shell->input_stack->name &&
         lexer->source_identity == shell->input_stack->identity);
}

static bool ash_control_invariants(const struct ash_control_state* control) {
    if (control->pending < ASH_CONTROL_NONE ||
        control->pending > ASH_CONTROL_RETURN) {
        return false;
    }
    switch (control->pending) {
        case ASH_CONTROL_NONE:
            return control->remaining_levels == 0u;
        case ASH_CONTROL_BREAK:
        case ASH_CONTROL_CONTINUE:
            return control->remaining_levels != 0u &&
                control->remaining_levels <= control->loop_depth;
        case ASH_CONTROL_RETURN:
            return control->remaining_levels == 0u &&
                control->function_depth != 0u;
    }
    return false;
}

static size_t ash_function_scope_count(const struct ash_shell* shell) {
    size_t count = 0u;
    for (const struct ash_scope* scope = shell->scopes;
         scope != NULL;
         scope = scope->parent) {
        count += scope->kind == ASH_SCOPE_FUNCTION;
    }
    return count;
}

static size_t ash_execution_function_count(const struct ash_shell* shell) {
    size_t count = 0u;
    for (const struct ash_execution_frame* frame =
             shell->execution_frames;
         frame != NULL;
         frame = frame->previous) {
        count += frame->kind == ASH_EXECUTION_FUNCTION_FRAME;
    }
    return count;
}

bool ash_shell_context_invariants(const struct ash_shell* shell) {
    bool standalone_applets = shell != NULL &&
        ash_shell_policy_has(
            &shell->policy,
            ASH_SHELL_POLICY_STANDALONE_APPLETS
        );
    bool interactive = shell != NULL &&
        ash_shell_policy_has(
            &shell->policy,
            ASH_SHELL_POLICY_INTERACTIVE
        );
    return shell != NULL &&
        shell->progname != NULL &&
        standalone_applets == shell->owns_self_executable_fd &&
        (shell->owns_self_executable_fd ?
            shell->self_executable_fd >= 0 :
            shell->self_executable_fd == -1) &&
        shell->shell_pid > 0 &&
        shell->last_async_pid >= -1 &&
        shell->command_substitution != NULL &&
        (shell->options & ~ASH_SHELL_OPTION_ALL) == 0u &&
        ash_shell_policy_valid(&shell->policy) &&
        ash_interactive_state_valid(&shell->interactive) &&
        interactive ==
            ash_interactive_state_enabled(&shell->interactive) &&
        ash_scope_stack_invariants(shell) &&
        ash_functions_invariants(shell) &&
        ash_input_stack_invariants(shell) &&
        ash_execution_trace_invariants(shell) &&
        ash_parser_state_invariants(shell) &&
        bx_fd_transaction_stack_invariants(&shell->redirections) &&
        ash_jobs_invariants(shell) &&
        ash_control_invariants(&shell->control) &&
        ash_function_scope_count(shell) ==
            shell->control.function_depth &&
        ash_execution_function_count(shell) ==
            shell->control.function_depth;
}

void ash_shell_context_assert_invariants(const struct ash_shell* shell) {
    (void)shell;
    assert(ash_shell_context_invariants(shell));
}

#ifndef NDEBUG
static bool ash_shell_context_empty(const struct ash_shell* shell) {
    return shell->progname == NULL &&
        shell->scopes == NULL &&
        shell->interactive.input == ASH_STARTUP_COMMAND_STRING &&
        shell->interactive.mode == ASH_INTERACTIVE_DISABLED &&
        shell->interactive.terminal_attachments ==
            ASH_TERMINAL_DETACHED &&
        shell->shopt == NULL &&
        shell->aliases == NULL &&
        shell->functions == NULL &&
        shell->traps == NULL &&
        shell->jobs == NULL &&
        shell->command_cache == NULL &&
        shell->input_stack == NULL &&
        shell->source_names == NULL &&
        shell->source_identities == NULL &&
        shell->execution_frames == NULL &&
        ash_source_location_is_none(&shell->execution_location) &&
        !shell->parser_state.active &&
        shell->parser_state.parser.lexer.source_name == NULL &&
        shell->parser_state.parser.lexer.source_identity == NULL &&
        shell->parser_state.parser.lexer.input == NULL &&
        shell->history == NULL &&
        shell->completion == NULL &&
        shell->cwd.physical == NULL &&
        shell->cwd.logical == NULL &&
        shell->cwd.old_logical == NULL &&
        shell->redirections.active == NULL &&
        shell->redirections.entries == NULL &&
        shell->redirections.entry_count == 0u &&
        shell->redirections.entry_capacity == 0u &&
        !shell->owns_self_executable_fd &&
        shell->shell_pid == 0 &&
        shell->command_substitution == NULL;
}
#endif

bool ash_shell_context_init(
    struct ash_shell* shell,
    const struct ash_shell_context_config* config
) {
    bool standalone_applets = config != NULL &&
        ash_shell_policy_has(
            &config->policy,
            ASH_SHELL_POLICY_STANDALONE_APPLETS
        );
    bool interactive = config != NULL &&
        ash_shell_policy_has(
            &config->policy,
            ASH_SHELL_POLICY_INTERACTIVE
        );
    if (shell == NULL || config == NULL ||
        config->progname == NULL || config->argv0 == NULL ||
        config->positional_count < 0 ||
        (config->positional_count > 0 &&
            config->positional_values == NULL) ||
        (config->options & ~ASH_SHELL_OPTION_ALL) != 0u ||
        !ash_shell_policy_valid(&config->policy) ||
        !ash_interactive_state_valid(&config->interactive) ||
        interactive !=
            ash_interactive_state_enabled(&config->interactive) ||
        standalone_applets != config->take_self_executable_fd ||
        (config->take_self_executable_fd &&
            config->self_executable_fd < 0) ||
        config->shell_pid <= 0 ||
        config->command_substitution == NULL) {
        return false;
    }

    struct ash_shell candidate = {
        .progname = config->progname,
        .options = config->options,
        .policy = config->policy,
        .interactive = config->interactive,
        .owns_self_executable_fd = config->take_self_executable_fd,
        .self_executable_fd = config->take_self_executable_fd ?
            config->self_executable_fd :
            -1,
        .shell_pid = config->shell_pid,
        .next_job_id = 1u,
        .last_async_pid = -1,
        .command_substitution = config->command_substitution,
    };
    if (!ash_scope_stack_init(
            &candidate,
            config->argv0,
            config->positional_values,
            (size_t)config->positional_count
        )) {
        return false;
    }
    bx_fd_transaction_stack_init(&candidate.redirections);
    assert(ash_shell_context_invariants(&candidate));
    *shell = candidate;
    return true;
}

struct ash_parser* ash_shell_context_begin_parse(
    struct ash_shell* shell,
    struct ash_source_location origin,
    const char* input,
    size_t length
) {
    if (shell == NULL || !ash_source_location_valid(&origin) ||
        input == NULL || origin.offset > SIZE_MAX - length ||
        shell->parser_state.active ||
        (shell->input_stack != NULL &&
         (origin.source != shell->input_stack->name ||
          origin.identity != shell->input_stack->identity))) {
        return NULL;
    }
    assert(ash_shell_context_invariants(shell));
    ash_parser_init_at(
        &shell->parser_state.parser,
        origin,
        input,
        length
    );
    shell->parser_state.active = true;
    assert(ash_shell_context_invariants(shell));
    return &shell->parser_state.parser;
}

void ash_shell_context_end_parse(struct ash_shell* shell) {
    if (shell == NULL || !shell->parser_state.active) {
        return;
    }
    assert(ash_shell_context_invariants(shell));
    ash_parser_destroy(&shell->parser_state.parser);
    shell->parser_state = (struct ash_parser_state){0};
    assert(ash_shell_context_invariants(shell));
}

void ash_shell_context_detach_after_fork(struct ash_shell* shell) {
    if (shell == NULL) {
        return;
    }
    assert(ash_shell_context_invariants(shell));
    ash_jobs_detach_after_fork(shell);
    bx_fd_transaction_stack_discard(&shell->redirections);
    assert(ash_shell_context_invariants(shell));
}

void ash_shell_option_letters(const struct ash_shell* shell, char* output, size_t output_size) {
    static const struct {
        uint32_t option;
        uint32_t policy_flag;
        char letter;
    } options[] = {
        {ASH_SHELL_OPTION_ALLEXPORT, 0u, 'a'},
        {ASH_SHELL_OPTION_NOTIFY, 0u, 'b'},
        {ASH_SHELL_OPTION_NOCLOBBER, 0u, 'C'},
        {ASH_SHELL_OPTION_ERREXIT, 0u, 'e'},
        {ASH_SHELL_OPTION_NOGLOB, 0u, 'f'},
        {0u, ASH_SHELL_POLICY_INTERACTIVE, 'i'},
        {ASH_SHELL_OPTION_MONITOR, 0u, 'm'},
        {ASH_SHELL_OPTION_NOEXEC, 0u, 'n'},
        {ASH_SHELL_OPTION_STDIN, 0u, 's'},
        {ASH_SHELL_OPTION_NOUNSET, 0u, 'u'},
        {ASH_SHELL_OPTION_VERBOSE, 0u, 'v'},
        {ASH_SHELL_OPTION_XTRACE, 0u, 'x'},
    };

    if (output_size == 0u) {
        return;
    }
    size_t length = 0u;
    for (size_t i = 0u; i < sizeof(options) / sizeof(options[0]); i++) {
        bool enabled = options[i].option != 0u ?
            (shell->options & options[i].option) != 0u :
            ash_shell_policy_has(
                &shell->policy,
                (enum ash_shell_policy_flag)options[i].policy_flag
            );
        if (enabled && length + 1u < output_size) {
            output[length++] = options[i].letter;
        }
    }
    output[length] = '\0';
}

void ash_shell_context_release_owned(struct ash_shell* shell) {
    if (shell == NULL) {
        return;
    }
    if (shell->progname == NULL) {
        assert(ash_shell_context_empty(shell));
        return;
    }
    assert(ash_shell_context_invariants(shell));
    ash_shell_context_end_parse(shell);
    ash_input_release_all(shell);
    ash_functions_destroy(shell);
    ash_jobs_destroy(shell);
    bx_fd_transaction_stack_discard(&shell->redirections);
    ash_input_source_registry_destroy(shell);
    ash_scope_stack_destroy(shell);
    if (shell->owns_self_executable_fd) {
        close(shell->self_executable_fd);
    }
    free(shell->cwd.physical);
    free(shell->cwd.logical);
    free(shell->cwd.old_logical);
    memset(shell, 0, sizeof(*shell));
    assert(ash_shell_context_empty(shell));
}
