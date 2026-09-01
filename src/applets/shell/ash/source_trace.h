#ifndef BX_APPLETS_SHELL_ASH_SOURCE_TRACE_H
#define BX_APPLETS_SHELL_ASH_SOURCE_TRACE_H

#include <stdbool.h>
#include <stddef.h>

#include "applets/shell/ash/syntax.h"

struct ash_shell;

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

enum ash_execution_frame_kind {
    ASH_EXECUTION_SOURCE_FRAME = 0,
    ASH_EXECUTION_FUNCTION_FRAME,
};

/*
 * A source identity represents one invocation, not merely one interned name.
 * It survives input withdrawal so ASTs and function definitions retain the
 * exact typed source and caller relationship that produced them.
 */
struct ash_source_identity {
    enum ash_input_kind kind;
    const char* name;
    struct ash_source_location caller;
    size_t reference_count;
    struct ash_source_identity* next;
    bool published;
};

/*
 * One interleaved stack is authoritative for nested sources and functions.
 * Source frames are embedded in their owning input sources. Function frames
 * borrow invocation-lifetime storage and therefore must be popped before the
 * synchronous invocation returns.
 */
struct ash_execution_frame {
    enum ash_execution_frame_kind kind;
    enum ash_input_kind source_kind;
    const char* function_name;
    struct ash_source_location entry;
    struct ash_source_location caller;
    struct ash_execution_frame* previous;
};

struct ash_execution_location_guard {
    struct ash_source_location previous;
    bool active;
};

bool ash_input_kind_valid(enum ash_input_kind kind);
bool ash_execution_trace_invariants(const struct ash_shell* shell);
bool ash_source_identity_is_owned(
    const struct ash_shell* shell,
    const struct ash_source_identity* identity
);
bool ash_source_identity_retain(struct ash_source_identity* identity);
void ash_source_identity_release(
    struct ash_shell* shell,
    struct ash_source_identity* identity
);

void ash_execution_push_source(
    struct ash_shell* shell,
    struct ash_execution_frame* frame,
    enum ash_input_kind source_kind,
    struct ash_source_location entry,
    struct ash_source_location caller
);
bool ash_execution_push_function(
    struct ash_shell* shell,
    struct ash_execution_frame* frame,
    const char* function_name,
    struct ash_source_location definition,
    struct ash_source_location caller
);
bool ash_execution_pop(
    struct ash_shell* shell,
    struct ash_execution_frame* frame
);

struct ash_source_location ash_execution_current_location(
    const struct ash_shell* shell
);
void ash_execution_location_enter(
    struct ash_shell* shell,
    struct ash_source_location location,
    struct ash_execution_location_guard* guard
);
void ash_execution_location_leave(
    struct ash_shell* shell,
    struct ash_execution_location_guard* guard
);

#endif /* BX_APPLETS_SHELL_ASH_SOURCE_TRACE_H */
