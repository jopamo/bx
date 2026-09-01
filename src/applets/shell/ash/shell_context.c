#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/input.h"
#include "applets/shell/ash/functions.h"
#include "applets/shell/ash/process.h"
#include "applets/shell/ash/scope.h"
#include "applets/shell/ash/shell_context.h"

bool ash_shell_context_init(
    struct ash_shell* shell,
    const struct ash_shell_context_config* config
) {
    if (shell == NULL || config == NULL ||
        config->progname == NULL || config->argv0 == NULL ||
        config->positional_count < 0 ||
        (config->positional_count > 0 &&
            config->positional_values == NULL) ||
        (config->options & ~ASH_SHELL_OPTION_ALL) != 0u ||
        !ash_shell_policy_valid(&config->policy) ||
        config->shell_pid <= 0 ||
        config->command_substitution == NULL) {
        return false;
    }

    struct ash_shell candidate = {
        .progname = config->progname,
        .options = config->options,
        .policy = config->policy,
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
    *shell = candidate;
    return true;
}

struct ash_parser* ash_shell_context_begin_parse(
    struct ash_shell* shell,
    const char* source_name,
    const char* input,
    size_t length
) {
    if (shell == NULL || source_name == NULL || input == NULL ||
        shell->parser_state.active) {
        return NULL;
    }
    ash_parser_init(
        &shell->parser_state.parser,
        source_name,
        input,
        length
    );
    shell->parser_state.active = true;
    return &shell->parser_state.parser;
}

void ash_shell_context_end_parse(struct ash_shell* shell) {
    if (shell == NULL || !shell->parser_state.active) {
        return;
    }
    ash_parser_destroy(&shell->parser_state.parser);
    shell->parser_state = (struct ash_parser_state){0};
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
    ash_shell_context_end_parse(shell);
    ash_input_release_all(shell);
    ash_functions_destroy(shell);
    ash_jobs_destroy(shell);
    ash_input_source_names_destroy(shell);
    ash_scope_stack_destroy(shell);
    free(shell->cwd.physical);
    free(shell->cwd.logical);
    free(shell->cwd.old_logical);
    memset(shell, 0, sizeof(*shell));
}
