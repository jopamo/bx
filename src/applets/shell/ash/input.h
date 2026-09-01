#ifndef BX_APPLETS_SHELL_ASH_INPUT_H
#define BX_APPLETS_SHELL_ASH_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#include "applets/shell/ash/source_trace.h"

struct ash_shell;
struct ash_source_name;
struct bx_text_buffer;

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
    /* Borrowed from the context-owned interned source-name pool. */
    const char* name;
    struct ash_source_name* name_record;
    struct ash_source_identity* identity;
    size_t physical_line;
    size_t logical_line;
    size_t byte_offset;
    size_t parser_offset;
    struct ash_execution_frame execution_frame;
    struct ash_input_source* previous;

    union {
        struct {
            /* Owned copy of the caller's input string. */
            char* text;
            size_t length;
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
void ash_input_source_registry_destroy(struct ash_shell* shell);
bool ash_input_stack_invariants(const struct ash_shell* shell);

/*
 * Replaces line with one complete physical input line. EOF and failure leave
 * it empty; errno distinguishes EOF (zero), allocation, I/O, and overflow.
 */
ssize_t ash_input_read_line(
    struct ash_shell* shell,
    struct bx_text_buffer* line
);
enum ash_input_kind ash_input_source_kind(const struct ash_shell* shell);
const char* ash_input_source_name(const struct ash_shell* shell);
size_t ash_input_source_physical_line(const struct ash_shell* shell);
size_t ash_input_source_logical_line(const struct ash_shell* shell);
size_t ash_input_source_parser_offset(const struct ash_shell* shell);
struct ash_source_location ash_input_next_location(
    const struct ash_shell* shell
);
bool ash_input_note_parse(
    struct ash_shell* shell,
    struct ash_source_location origin,
    size_t parser_offset
);
bool ash_input_source_has_error(const struct ash_shell* shell);

#endif /* BX_APPLETS_SHELL_ASH_INPUT_H */
