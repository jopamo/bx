#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/command.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/input.h"
#include "applets/shell/ash/input_builtins.h"
#include "applets/shell/ash/input_execution.h"
#include "applets/shell/ash/scope.h"
#include "applets/shell/ash/variables.h"
#include "lib/text_buffer.h"

int ash_input_builtin_eval(
    struct ash_shell* shell,
    const struct ash_command* command
) {
    if (command->word_count == 1u) {
        return 0;
    }

    struct bx_text_buffer input;
    bx_text_buffer_init(&input);
    for (size_t i = 1u; i < command->word_count; i++) {
        if (i != 1u && !bx_text_buffer_append_char(&input, ' ')) {
            bx_text_buffer_destroy(&input);
            ash_diag_oom(shell);
            return 2;
        }
        const char* word = command->words[i];
        if (!bx_text_buffer_append_span(&input, word, strlen(word))) {
            bx_text_buffer_destroy(&input);
            ash_diag_oom(shell);
            return 2;
        }
    }

    int status = ash_input_execute_string(
        shell,
        ASH_INPUT_EVAL,
        NULL,
        input.data != NULL ? input.data : "",
        input.length
    );
    bx_text_buffer_destroy(&input);
    return status;
}

static char* ash_source_candidate(
    struct ash_shell* shell,
    const char* directory,
    size_t directory_length,
    const char* operand
) {
    size_t operand_length = strlen(operand);
    bool add_slash = directory_length != 0u &&
        directory[directory_length - 1u] != '/';
    if (directory_length > SIZE_MAX - operand_length) {
        errno = ENOMEM;
        ash_diag_oom(shell);
        return NULL;
    }

    size_t length = directory_length + operand_length;
    if (length == SIZE_MAX ||
        (add_slash && length == SIZE_MAX - 1u)) {
        errno = ENOMEM;
        ash_diag_oom(shell);
        return NULL;
    }
    if (add_slash) {
        length++;
    }

    char* candidate = malloc(length + 1u);
    if (candidate == NULL) {
        errno = ENOMEM;
        ash_diag_oom(shell);
        return NULL;
    }
    if (directory_length != 0u) {
        memcpy(candidate, directory, directory_length);
    }
    size_t offset = directory_length;
    if (add_slash) {
        candidate[offset++] = '/';
    }
    memcpy(candidate + offset, operand, operand_length + 1u);
    return candidate;
}

static FILE* ash_source_open(
    struct ash_shell* shell,
    const char* operand,
    char** searched_path_out
) {
    *searched_path_out = NULL;
    if (strchr(operand, '/') != NULL) {
        return fopen(operand, "r");
    }

    const char* path = ash_var_get(shell, "PATH");
    if (path == NULL) {
        path = "";
    }
    int best_error = ENOENT;
    const char* segment = path;
    for (;;) {
        const char* separator = strchr(segment, ':');
        size_t segment_length = separator != NULL ?
            (size_t)(separator - segment) :
            strlen(segment);
        char* candidate = ash_source_candidate(
            shell,
            segment,
            segment_length,
            operand
        );
        if (candidate == NULL) {
            return NULL;
        }

        FILE* stream = fopen(candidate, "r");
        if (stream != NULL) {
            *searched_path_out = candidate;
            return stream;
        }
        if (errno != ENOENT && errno != ENOTDIR) {
            best_error = errno;
        }
        free(candidate);
        if (separator == NULL) {
            break;
        }
        segment = separator + 1;
    }
    errno = best_error;
    return NULL;
}

int ash_input_builtin_source(
    struct ash_shell* shell,
    const struct ash_command* command
) {
    const char* builtin_name = command->words[0];
    if (command->word_count < 2u) {
        ash_diag(shell, "%s: filename argument required", builtin_name);
        return 2;
    }

    char* searched_path = NULL;
    FILE* stream = ash_source_open(
        shell,
        command->words[1],
        &searched_path
    );
    if (stream == NULL) {
        if (errno != ENOMEM) {
            ash_exec_error(shell, command->words[1], errno);
        }
        return errno == ENOMEM ? 2 : 1;
    }

    struct ash_positional_frame* positionals =
        ash_scope_positionals_mut(shell);
    struct ash_positional_frame saved_positionals = {0};
    bool override_positionals = command->word_count > 2u;
    if (override_positionals) {
        saved_positionals = *positionals;
        positionals->values = command->words + 2u;
        positionals->count = command->word_count - 2u;
    }

    int status = ash_input_execute_stream(
        shell,
        ASH_INPUT_SOURCED_FILE,
        searched_path != NULL ? searched_path : command->words[1],
        stream,
        ASH_INPUT_TAKE_STREAM,
        false
    );
    if (override_positionals) {
        *positionals = saved_positionals;
    }
    free(searched_path);
    return status;
}
