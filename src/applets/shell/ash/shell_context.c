#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "applets/shell/ash/shell_context.h"

void ash_shell_option_letters(const struct ash_shell* shell, char* output, size_t output_size) {
    static const struct {
        uint32_t option;
        char letter;
    } options[] = {
        {ASH_SHELL_OPTION_ALLEXPORT, 'a'},
        {ASH_SHELL_OPTION_NOTIFY, 'b'},
        {ASH_SHELL_OPTION_NOCLOBBER, 'C'},
        {ASH_SHELL_OPTION_ERREXIT, 'e'},
        {ASH_SHELL_OPTION_NOGLOB, 'f'},
        {ASH_SHELL_OPTION_INTERACTIVE, 'i'},
        {ASH_SHELL_OPTION_MONITOR, 'm'},
        {ASH_SHELL_OPTION_NOEXEC, 'n'},
        {ASH_SHELL_OPTION_STDIN, 's'},
        {ASH_SHELL_OPTION_NOUNSET, 'u'},
        {ASH_SHELL_OPTION_VERBOSE, 'v'},
        {ASH_SHELL_OPTION_XTRACE, 'x'},
    };

    if (output_size == 0u) {
        return;
    }
    size_t length = 0u;
    for (size_t i = 0u; i < sizeof(options) / sizeof(options[0]); i++) {
        if ((shell->options & options[i].option) != 0u && length + 1u < output_size) {
            output[length++] = options[i].letter;
        }
    }
    output[length] = '\0';
}

void ash_shell_context_release_owned(struct ash_shell* shell) {
    free(shell->cwd.physical);
    free(shell->cwd.logical);
    free(shell->cwd.old_logical);
    shell->cwd = (struct ash_cwd_state){0};
}
