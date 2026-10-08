#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets/shell/ash/ast.h"
#include "applets/shell/ash/control.h"
#include "applets/shell/ash/diagnostic.h"
#include "applets/shell/ash/executor.h"
#include "applets/shell/ash/input.h"
#include "applets/shell/ash/input_execution.h"
#include "applets/shell/ash/parser.h"
#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/traps.h"
#include "applets/shell/ash/variables.h"
#include "lib/text_buffer.h"

enum ash_input_boundary {
    ASH_INPUT_BOUNDARY_PHYSICAL_LINE = 0,
    ASH_INPUT_BOUNDARY_SOURCE_END,
};

enum ash_input_unit_state {
    ASH_INPUT_UNIT_EXECUTED = 0,
    ASH_INPUT_UNIT_NEEDS_MORE,
    ASH_INPUT_UNIT_PARSE_ERROR,
};

struct ash_input_unit_result {
    enum ash_input_unit_state state;
    int status;
};

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

struct ash_input_parser {
    struct ash_parser storage[2];
    struct ash_parser* checkpoint;
};

static void ash_input_parser_clear(struct ash_input_parser* state) {
    if (state->checkpoint != NULL) {
        ash_parser_destroy(state->checkpoint);
        *state->checkpoint = (struct ash_parser){0};
        state->checkpoint = NULL;
    }
}

static bool ash_input_onecmd_boundary(const struct ash_shell* shell) {
    if ((shell->options & ASH_SHELL_OPTION_ONECMD) == 0u) {
        return false;
    }
    switch (ash_input_source_kind(shell)) {
        case ASH_INPUT_STDIN:
        case ASH_INPUT_INTERACTIVE:
        case ASH_INPUT_SCRIPT_FILE:
            return true;
        case ASH_INPUT_INVALID:
        case ASH_INPUT_COMMAND_STRING:
        case ASH_INPUT_SOURCED_FILE:
        case ASH_INPUT_EVAL:
        case ASH_INPUT_TRAP:
        case ASH_INPUT_COMMAND_SUBSTITUTION:
        case ASH_INPUT_PROMPT_COMMAND:
        case ASH_INPUT_COMPLETION_HOOK:
            return false;
    }
    return false;
}

static struct ash_input_unit_result ash_input_parse_error(struct ash_shell* shell) {
    shell->last_status = 2;
    if (ash_shell_policy_noninteractive_posix(&shell->policy)) {
        shell->should_exit = true;
        shell->requested_exit_status = 2;
        if (ash_input_source_kind(shell) != ASH_INPUT_TRAP) {
            ash_trap_exit_override_status(shell);
        }
    }
    return (struct ash_input_unit_result){.state = ASH_INPUT_UNIT_PARSE_ERROR, .status = 2};
}

static struct ash_input_unit_result ash_input_execute_buffer(struct ash_shell* shell,
                                                             struct ash_input_parser* state,
                                                             struct ash_source_location origin,
                                                             const char* input,
                                                             size_t length,
                                                             enum ash_input_boundary boundary) {
    int status = shell->last_status;
    while (!shell->should_exit && !ash_control_pending(shell)) {
        struct ash_parser* parser = state->checkpoint == &state->storage[0] ? &state->storage[1] : &state->storage[0];
        if (state->checkpoint == NULL) {
            if (ash_shell_context_begin_parse(shell, parser, origin, input, length) == NULL) {
                ash_diag(shell, "parser state is already active");
                return ash_input_parse_error(shell);
            }
        }
        else {
            if (!ash_parser_clone_boundary(parser, state->checkpoint)) {
                ash_exec_error(shell, "parser checkpoint", errno);
                return ash_input_parse_error(shell);
            }
            if (!ash_shell_context_resume_parse(shell, parser)) {
                ash_parser_destroy(parser);
                *parser = (struct ash_parser){0};
                ash_diag(shell, "invalid parser checkpoint binding");
                return ash_input_parse_error(shell);
            }
        }
        struct ash_ast* command = NULL;
        enum ash_parser_result result = ash_parser_parse_complete_command(parser, &command);
        struct ash_source_location parser_position = ash_lexer_current_location(&parser->lexer);
        bool line_continuation = ash_lexer_ended_with_line_continuation(&parser->lexer);
        for (size_t i = 0u; i < parser->alias_frame_count; i++) {
            line_continuation |= ash_lexer_ended_with_line_continuation(&parser->alias_frames[i].lexer);
        }
        if (!ash_input_note_parse(shell, origin, parser_position.offset)) {
            ash_diag(shell, "invalid parser source position");
            ash_shell_context_end_parse(shell);
            ash_ast_destroy(command);
            return ash_input_parse_error(shell);
        }
        if ((result == ASH_PARSER_INCOMPLETE || (result == ASH_PARSER_COMPLETE && line_continuation)) && boundary == ASH_INPUT_BOUNDARY_PHYSICAL_LINE) {
            ash_shell_context_end_parse(shell);
            ash_ast_destroy(command);
            return (struct ash_input_unit_result){.state = ASH_INPUT_UNIT_NEEDS_MORE, .status = status};
        }
        if (result != ASH_PARSER_COMPLETE) {
            ash_diag_parse(shell, parser->error_location, "%s", ash_input_parser_diagnostic(parser->error));
            ash_shell_context_end_parse(shell);
            ash_ast_destroy(command);
            return ash_input_parse_error(shell);
        }
        if (command == NULL) {
            ash_shell_context_end_parse(shell);
            break;
        }
        if (line_continuation) {
            ash_shell_context_end_parse(shell);
            ash_input_parser_clear(state);
        }
        else {
            if (ash_shell_context_suspend_parse(shell) == NULL) {
                ash_diag(shell, "invalid parser command boundary");
                ash_shell_context_end_parse(shell);
                ash_ast_destroy(command);
                return ash_input_parse_error(shell);
            }
            ash_input_parser_clear(state);
            state->checkpoint = parser;
        }
        status = ash_execute_ast(shell, command);
        ash_ast_destroy(command);
        /* Nested eval/source input consumes its own unit discard. */
        bool discard = ash_control_consume_unit_discard(shell, &status);
        ash_control_publish_status(shell, status);
        if (discard || line_continuation || ash_input_onecmd_boundary(shell)) {
            break;
        }
    }
    return (struct ash_input_unit_result){.state = ASH_INPUT_UNIT_EXECUTED, .status = status};
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
    fputs(prompt, stderr);
    fflush(stderr);
}

static void ash_input_print_verbose(
    struct ash_shell* shell,
    const struct bx_text_buffer* line
) {
    if ((shell->options & ASH_SHELL_OPTION_VERBOSE) == 0u) {
        return;
    }
    bool needs_newline =
        line->length == 0u ||
        line->data[line->length - 1u] != '\n';
    if ((line->length != 0u &&
         fwrite(line->data, 1u, line->length, stderr) != line->length) ||
        (needs_newline && fputc('\n', stderr) == EOF) ||
        fflush(stderr) == EOF) {
        /* Failed echo does not discard input or determine command status. */
        clearerr(stderr);
    }
}

static int ash_input_execute_current(
    struct ash_shell* shell,
    bool prompt
) {
    int status = 0;
    struct bx_text_buffer logical_input;
    struct bx_text_buffer physical_line;
    bx_text_buffer_init(&logical_input);
    bx_text_buffer_init(&physical_line);
    struct ash_source_location pending_origin = {0};
    struct ash_input_parser parser = {0};

    while (!shell->should_exit && !ash_control_pending(shell)) {
        bool awaiting_more_input = logical_input.length != 0u;
        if (prompt && !awaiting_more_input) {
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
            ash_input_print_prompt(shell, awaiting_more_input);
        }

        struct ash_source_location line_origin =
            ash_input_next_location(shell);
        bool extending_logical_input = logical_input.length != 0u;
        struct bx_text_buffer* read_buffer =
            extending_logical_input ?
                &physical_line :
                &logical_input;
        ssize_t read_length = ash_input_read_line(
            shell,
            read_buffer
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
            else if (logical_input.length != 0u) {
                struct ash_input_unit_result result = ash_input_execute_buffer(shell, &parser, pending_origin, logical_input.data, logical_input.length, ASH_INPUT_BOUNDARY_SOURCE_END);
                assert(result.state != ASH_INPUT_UNIT_NEEDS_MORE);
                status = result.status;
            }
            break;
        }

        if (!ash_source_location_valid(&line_origin)) {
            ash_diag(shell, "input source position overflow");
            status = 2;
            break;
        }
        ash_input_print_verbose(shell, read_buffer);
        if (!extending_logical_input) {
            pending_origin = line_origin;
        }
        else {
            assert((size_t)read_length == physical_line.length);
            struct bx_text_buffer grown;
            bx_text_buffer_init(&grown);
            struct bx_text_buffer* target = parser.checkpoint != NULL ? &grown : &logical_input;
            bool appended = (parser.checkpoint == NULL || bx_text_buffer_append_span(target, logical_input.data, logical_input.length)) &&
                            bx_text_buffer_append_span(target, physical_line.data, physical_line.length);
            if (appended && parser.checkpoint != NULL) {
                appended = ash_parser_extend_input(parser.checkpoint, target->data, target->length);
            }
            if (!appended) {
                int error = errno;
                bx_text_buffer_destroy(&grown);
                if (error == EOVERFLOW || error == EINVAL) {
                    ash_exec_error(shell, "logical input", error);
                }
                else {
                    ash_diag_oom(shell);
                }
                status = 2;
                break;
            }
            if (parser.checkpoint != NULL) {
                bx_text_buffer_destroy(&logical_input);
                logical_input = grown;
            }
        }

        struct ash_input_unit_result result = ash_input_execute_buffer(shell, &parser, pending_origin, logical_input.data, logical_input.length, ASH_INPUT_BOUNDARY_PHYSICAL_LINE);
        status = result.status;
        if (result.state == ASH_INPUT_UNIT_NEEDS_MORE) {
            continue;
        }

        bool onecmd_boundary = ash_input_onecmd_boundary(shell);
        ash_input_parser_clear(&parser);
        bx_text_buffer_clear(&logical_input);
        pending_origin = (struct ash_source_location){0};
        if (result.state == ASH_INPUT_UNIT_PARSE_ERROR &&
            !ash_shell_policy_has(
                &shell->policy,
                ASH_SHELL_POLICY_INTERACTIVE
            )) {
            break;
        }
        if (onecmd_boundary) {
            break;
        }
    }

    ash_input_parser_clear(&parser);
    bx_text_buffer_destroy(&physical_line);
    bx_text_buffer_destroy(&logical_input);
    if (ash_input_source_kind(shell) == ASH_INPUT_SOURCED_FILE) {
        (void)ash_control_consume_return(shell, &status);
    }
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
    bool sourced = kind == ASH_INPUT_SOURCED_FILE;
    shell->exit_trap_defer_depth += sourced ? 1u : 0u;
    int status = ash_input_execute_current(shell, false);
    ash_input_pop(shell);
    shell->exit_trap_defer_depth -= sourced ? 1u : 0u;
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
    if (stream != NULL &&
        (kind == ASH_INPUT_STDIN ||
         kind == ASH_INPUT_INTERACTIVE ||
         kind == ASH_INPUT_SCRIPT_FILE)) {
        /* Main input must leave bytes after the current line for commands. */
        errno = 0;
        if (setvbuf(stream, NULL, _IONBF, 0u) != 0) {
            int error = errno != 0 ? errno : EIO;
            if (ownership == ASH_INPUT_TAKE_STREAM) {
                fclose(stream);
            }
            ash_exec_error(shell, "input buffering", error);
            return 2;
        }
    }
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
    bool sourced = kind == ASH_INPUT_SOURCED_FILE;
    shell->exit_trap_defer_depth += sourced ? 1u : 0u;
    int status = ash_input_execute_current(shell, prompt);
    ash_input_pop(shell);
    shell->exit_trap_defer_depth -= sourced ? 1u : 0u;
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
