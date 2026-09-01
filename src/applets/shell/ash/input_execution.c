#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets/shell/ash/ast.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/executor.h"
#include "applets/shell/ash/input.h"
#include "applets/shell/ash/input_execution.h"
#include "applets/shell/ash/parser.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/variables.h"
#include "lib/text_buffer.h"

static const char* ash_input_parser_diagnostic(const char* diagnostic) {
    if (diagnostic == NULL) {
        return "syntax error";
    }
    if (strcmp(diagnostic, "unterminated parameter expansion") == 0) {
        return "bad substitution";
    }
    if (strcmp(diagnostic, "command expected after pipe") == 0) {
        return "syntax error near unexpected token '|'";
    }
    if (strcmp(diagnostic, "redirection target expected") == 0 ||
        strcmp(diagnostic, "redirection target must be a word") == 0) {
        return "redirection requires a target";
    }
    return diagnostic;
}

static int ash_input_execute_buffer(
    struct ash_shell* shell,
    struct ash_source_location origin,
    const char* input,
    size_t length,
    bool final_input,
    bool* incomplete_out,
    bool* parser_error_out
) {
    *incomplete_out = false;
    *parser_error_out = false;

    struct ash_parser* parser = ash_shell_context_begin_parse(
        shell,
        origin,
        input,
        length
    );
    if (parser == NULL) {
        ash_diag(shell, "parser state is already active");
        *parser_error_out = true;
        shell->last_status = 2;
        return 2;
    }
    struct ash_ast* program = NULL;
    enum ash_parser_result result = ash_parser_parse_program(
        parser,
        &program
    );
    struct ash_source_location parser_position =
        ash_lexer_current_location(&parser->lexer);
    if (!ash_input_note_parse(
            shell,
            origin,
            parser_position.offset
        )) {
        ash_diag(shell, "invalid parser source position");
        ash_shell_context_end_parse(shell);
        ash_ast_destroy(program);
        *parser_error_out = true;
        shell->last_status = 2;
        return 2;
    }
    if (result == ASH_PARSER_INCOMPLETE && !final_input) {
        *incomplete_out = true;
        ash_shell_context_end_parse(shell);
        return shell->last_status;
    }
    if (result != ASH_PARSER_COMPLETE) {
        ash_diag(
            shell,
            "%s",
            ash_input_parser_diagnostic(parser->error)
        );
        *parser_error_out = true;
        ash_shell_context_end_parse(shell);
        shell->last_status = 2;
        return 2;
    }

    ash_shell_context_end_parse(shell);
    int status = ash_execute_ast(shell, program);
    shell->last_status = status;
    ash_ast_destroy(program);
    return status;
}

const char* ash_input_default_prompt(void) {
    return geteuid() == 0 ? "# " : "$ ";
}

static void ash_input_print_prompt(
    struct ash_shell* shell,
    bool continuation
) {
    const char* prompt = ash_var_get(
        shell,
        continuation ? "PS2" : "PS1"
    );
    if (prompt == NULL) {
        prompt = continuation ? "> " : ash_input_default_prompt();
    }
    fputs(prompt, stdout);
    fflush(stdout);
}

static int ash_input_execute_current(
    struct ash_shell* shell,
    bool prompt
) {
    char* line = NULL;
    size_t capacity = 0u;
    int status = 0;
    struct bx_text_buffer pending;
    bx_text_buffer_init(&pending);
    struct ash_source_location pending_origin = {0};
    bool continuation = false;

    while (!shell->should_exit) {
        if (prompt && !continuation) {
            const char* prompt_command = ash_var_get(
                shell,
                "PROMPT_COMMAND"
            );
            if (prompt_command != NULL && prompt_command[0] != '\0') {
                status = ash_input_execute_hook(
                    shell,
                    ASH_INPUT_PROMPT_COMMAND,
                    prompt_command
                );
                if (shell->should_exit) {
                    break;
                }
            }
        }
        if (prompt) {
            ash_input_print_prompt(shell, continuation);
        }

        errno = 0;
        struct ash_source_location line_origin =
            ash_input_next_location(shell);
        ssize_t read_length = ash_input_read_line(
            shell,
            &line,
            &capacity
        );
        if (read_length < 0) {
            bool read_error = ash_input_source_has_error(shell) ||
                errno != 0;
            if (read_error) {
                ash_exec_error(
                    shell,
                    "getline",
                    errno != 0 ? errno : EIO
                );
                status = 1;
            }
            else if (pending.length != 0u) {
                bool incomplete = false;
                bool parser_error = false;
                status = ash_input_execute_buffer(
                    shell,
                    pending_origin,
                    pending.data,
                    pending.length,
                    true,
                    &incomplete,
                    &parser_error
                );
                (void)incomplete;
            }
            break;
        }

        if (!ash_source_location_valid(&line_origin)) {
            ash_diag(shell, "input source position overflow");
            status = 2;
            break;
        }
        if (pending.length == 0u) {
            pending_origin = line_origin;
        }
        if (!bx_text_buffer_append_span(
                &pending,
                line,
                (size_t)read_length
            )) {
            ash_diag_oom(shell);
            status = 2;
            break;
        }

        bool incomplete = false;
        bool parser_error = false;
        status = ash_input_execute_buffer(
            shell,
            pending_origin,
            pending.data,
            pending.length,
            false,
            &incomplete,
            &parser_error
        );
        if (incomplete) {
            continuation = true;
            continue;
        }

        bx_text_buffer_clear(&pending);
        pending_origin = (struct ash_source_location){0};
        continuation = false;
        if (parser_error &&
            !ash_shell_policy_has(
                &shell->policy,
                ASH_SHELL_POLICY_INTERACTIVE
            )) {
            break;
        }
    }

    bx_text_buffer_destroy(&pending);
    free(line);
    return status;
}

int ash_input_execute_string(
    struct ash_shell* shell,
    enum ash_input_kind kind,
    const char* name,
    const char* text,
    size_t length
) {
    if (!ash_input_push_string_span(
            shell,
            kind,
            name,
            text,
            length
        )) {
        ash_diag_oom(shell);
        return 2;
    }
    int status = ash_input_execute_current(shell, false);
    ash_input_pop(shell);
    return status;
}

int ash_input_execute_stream(
    struct ash_shell* shell,
    enum ash_input_kind kind,
    const char* name,
    FILE* stream,
    enum ash_input_stream_ownership ownership,
    bool prompt
) {
    if (!ash_input_push_file(
            shell,
            kind,
            name,
            stream,
            ownership
        )) {
        if (ownership == ASH_INPUT_TAKE_STREAM && stream != NULL) {
            fclose(stream);
        }
        ash_diag_oom(shell);
        return 2;
    }
    int status = ash_input_execute_current(shell, prompt);
    ash_input_pop(shell);
    return status;
}

int ash_input_execute_hook(
    struct ash_shell* shell,
    enum ash_input_kind kind,
    const char* command
) {
    if ((kind != ASH_INPUT_PROMPT_COMMAND &&
         kind != ASH_INPUT_COMPLETION_HOOK) ||
        command == NULL) {
        errno = EINVAL;
        return 2;
    }
    return ash_input_execute_string(
        shell,
        kind,
        NULL,
        command,
        strlen(command)
    );
}
