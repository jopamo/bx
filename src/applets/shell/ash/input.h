#ifndef BX_APPLETS_SHELL_ASH_INPUT_H
#define BX_APPLETS_SHELL_ASH_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

struct ash_shell;
struct ash_source_name;

enum ash_input_kind {
    ASH_INPUT_INVALID = 0,
    ASH_INPUT_COMMAND_STRING,
    ASH_INPUT_STDIN,
    ASH_INPUT_SCRIPT_FILE,
    ASH_INPUT_SOURCED_FILE,
    ASH_INPUT_EVAL,
    ASH_INPUT_COMMAND_SUBSTITUTION,
    ASH_INPUT_PROMPT_COMMAND,
    ASH_INPUT_COMPLETION_HOOK,
    ASH_INPUT_INTERACTIVE,
};

enum ash_input_transport {
    ASH_INPUT_TRANSPORT_STRING = 0,
    ASH_INPUT_TRANSPORT_FILE,
};

enum ash_input_stream_ownership {
    ASH_INPUT_BORROW_STREAM = 0,
    ASH_INPUT_TAKE_STREAM,
};

struct ash_input_source {
    enum ash_input_kind kind;
    enum ash_input_transport transport;
    /* Borrowed from the context-owned source identity pool. */
    const char* name;
    struct ash_source_name* identity;
    size_t line;
    struct ash_input_source* previous;

    union {
        struct {
            /* Owned copy of the caller's input string. */
            char* text;
            size_t length;
            size_t offset;
        } string;
        struct {
            FILE* stream;
            enum ash_input_stream_ownership ownership;
        } file;
    } source;
};

bool ash_input_push_string_span(
    struct ash_shell* shell,
    enum ash_input_kind kind,
    const char* name,
    const char* text,
    size_t length
);
/*
 * Stream ownership transfers only after a successful push. A taken stream is
 * closed on pop; a borrowed stream remains the caller's responsibility.
 */
bool ash_input_push_file(
    struct ash_shell* shell,
    enum ash_input_kind kind,
    const char* name,
    FILE* stream,
    enum ash_input_stream_ownership ownership
);
void ash_input_pop(struct ash_shell* shell);
void ash_input_release_all(struct ash_shell* shell);
void ash_input_source_names_destroy(struct ash_shell* shell);
bool ash_input_stack_invariants(const struct ash_shell* shell);

ssize_t ash_input_read_line(struct ash_shell* shell, char** line, size_t* capacity);
enum ash_input_kind ash_input_source_kind(const struct ash_shell* shell);
const char* ash_input_source_name(const struct ash_shell* shell);
size_t ash_input_source_line(const struct ash_shell* shell);
bool ash_input_source_has_error(const struct ash_shell* shell);

#endif /* BX_APPLETS_SHELL_ASH_INPUT_H */
