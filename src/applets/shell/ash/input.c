#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/input.h"
#include "applets/shell/ash/shell_context.h"

struct ash_source_name {
    char* value;
    struct ash_source_name* next;
    bool published;
};

static bool ash_input_chain_acyclic(const struct ash_input_source* input) {
    const struct ash_input_source* slow = input;
    const struct ash_input_source* fast = input;
    while (fast != NULL && fast->previous != NULL) {
        slow = slow->previous;
        fast = fast->previous->previous;
        if (slow == fast) {
            return false;
        }
    }
    return true;
}

static bool ash_source_name_chain_acyclic(
    const struct ash_source_name* identity
) {
    const struct ash_source_name* slow = identity;
    const struct ash_source_name* fast = identity;
    while (fast != NULL && fast->next != NULL) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            return false;
        }
    }
    return true;
}

static bool ash_source_identity_chain_acyclic(
    const struct ash_source_identity* identity
) {
    const struct ash_source_identity* slow = identity;
    const struct ash_source_identity* fast = identity;
    while (fast != NULL && fast->next != NULL) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            return false;
        }
    }
    return true;
}

static bool ash_source_name_is_owned(
    const struct ash_shell* shell,
    const struct ash_source_name* identity
) {
    for (const struct ash_source_name* current = shell->source_names;
         current != NULL;
         current = current->next) {
        if (current == identity) {
            return true;
        }
    }
    return false;
}

static const char* ash_input_default_name(enum ash_input_kind kind) {
    switch (kind) {
        case ASH_INPUT_INVALID:
            break;
        case ASH_INPUT_COMMAND_STRING:
            return "-c";
        case ASH_INPUT_STDIN:
        case ASH_INPUT_INTERACTIVE:
            return "<stdin>";
        case ASH_INPUT_SCRIPT_FILE:
            return "<script>";
        case ASH_INPUT_SOURCED_FILE:
            return "<source>";
        case ASH_INPUT_EVAL:
            return "eval";
        case ASH_INPUT_COMMAND_SUBSTITUTION:
            return "<command substitution>";
        case ASH_INPUT_PROMPT_COMMAND:
            return "<PROMPT_COMMAND>";
        case ASH_INPUT_COMPLETION_HOOK:
            return "<completion>";
    }
    return "<input>";
}

static bool ash_source_caller_chain_acyclic(
    const struct ash_source_identity* identity
) {
    const struct ash_source_identity* slow = identity;
    const struct ash_source_identity* fast = identity;
    while (fast != NULL && fast->caller.identity != NULL) {
        slow = slow->caller.identity;
        fast = fast->caller.identity->caller.identity;
        if (slow == fast) {
            return false;
        }
    }
    return true;
}

bool ash_input_stack_invariants(const struct ash_shell* shell) {
    if (shell == NULL ||
        !ash_input_chain_acyclic(shell->input_stack) ||
        !ash_source_name_chain_acyclic(shell->source_names) ||
        !ash_source_identity_chain_acyclic(
            shell->source_identities
        ) ||
        !ash_execution_trace_invariants(shell)) {
        return false;
    }

    for (const struct ash_source_name* identity = shell->source_names;
         identity != NULL;
         identity = identity->next) {
        if (!identity->published || identity->value == NULL) {
            return false;
        }
    }
    for (const struct ash_source_identity* identity =
             shell->source_identities;
         identity != NULL;
         identity = identity->next) {
        if (!identity->published ||
            !ash_input_kind_valid(identity->kind) ||
            identity->name == NULL ||
            identity->reference_count == 0u ||
            (ash_source_location_is_none(&identity->caller) !=
             (identity->caller.identity == NULL)) ||
            (!ash_source_location_is_none(&identity->caller) &&
             (!ash_source_location_valid(&identity->caller) ||
              identity->caller.identity == identity ||
              !ash_source_identity_is_owned(
                  shell,
                  identity->caller.identity
              ) ||
              identity->caller.source !=
                  identity->caller.identity->name))) {
            return false;
        }
    }
    for (const struct ash_source_identity* identity =
             shell->source_identities;
         identity != NULL;
         identity = identity->next) {
        if (!ash_source_caller_chain_acyclic(identity)) {
            return false;
        }
    }
    for (const struct ash_source_name* identity = shell->source_names;
         identity != NULL;
         identity = identity->next) {
        for (const struct ash_source_name* duplicate = identity->next;
             duplicate != NULL;
             duplicate = duplicate->next) {
            if (strcmp(identity->value, duplicate->value) == 0) {
                return false;
            }
        }
    }

    size_t input_count = 0u;
    for (const struct ash_input_source* input = shell->input_stack;
         input != NULL;
         input = input->previous) {
        input_count++;
        if (!ash_input_kind_valid(input->kind) ||
            (input->transport != ASH_INPUT_TRANSPORT_STRING &&
             input->transport != ASH_INPUT_TRANSPORT_FILE) ||
            input->name == NULL || input->name_record == NULL ||
            input->identity == NULL ||
            !ash_source_name_is_owned(shell, input->name_record) ||
            !ash_source_identity_is_owned(shell, input->identity) ||
            !input->name_record->published ||
            !input->identity->published ||
            input->name != input->name_record->value ||
            input->name != input->identity->name ||
            input->identity->kind != input->kind ||
            input->name_record->value == NULL) {
            return false;
        }
        const struct ash_execution_frame* frame =
            &input->execution_frame;
        if (frame->kind != ASH_EXECUTION_SOURCE_FRAME ||
            frame->source_kind != input->kind ||
            frame->function_name != NULL ||
            frame->entry.source != input->name ||
            frame->entry.identity != input->identity ||
            frame->caller.source != input->identity->caller.source ||
            frame->caller.identity !=
                input->identity->caller.identity ||
            frame->caller.line != input->identity->caller.line ||
            frame->caller.column != input->identity->caller.column ||
            frame->caller.offset != input->identity->caller.offset ||
            frame->entry.line != 1u ||
            frame->entry.column != 1u ||
            frame->entry.offset != 0u ||
            input->logical_line > input->physical_line ||
            input->parser_offset > input->byte_offset) {
            return false;
        }
        bool frame_published = false;
        for (const struct ash_execution_frame* published =
                 shell->execution_frames;
             published != NULL;
             published = published->previous) {
            if (published == frame) {
                frame_published = true;
                break;
            }
        }
        if (!frame_published) {
            return false;
        }
        if (input->transport == ASH_INPUT_TRANSPORT_STRING) {
            if (input->source.string.text == NULL ||
                input->byte_offset >
                    input->source.string.length) {
                return false;
            }
        }
        else if (input->source.file.stream == NULL ||
            (input->source.file.ownership != ASH_INPUT_BORROW_STREAM &&
             input->source.file.ownership != ASH_INPUT_TAKE_STREAM)) {
            return false;
        }
    }
    size_t source_frame_count = 0u;
    for (const struct ash_execution_frame* frame = shell->execution_frames;
         frame != NULL;
         frame = frame->previous) {
        source_frame_count += frame->kind == ASH_EXECUTION_SOURCE_FRAME;
    }
    return input_count == source_frame_count;
}

static char* ash_input_duplicate_span(const char* text, size_t length) {
    if (length == SIZE_MAX) {
        errno = ENOMEM;
        return NULL;
    }

    char* copy = malloc(length + 1u);
    if (copy == NULL) {
        return NULL;
    }
    if (length != 0u) {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';
    return copy;
}

static struct ash_input_source* ash_input_create(
    struct ash_shell* shell,
    enum ash_input_kind kind,
    enum ash_input_transport transport,
    const char* name
) {
    const char* effective_name = name != NULL ?
        name :
        ash_input_default_name(kind);
    struct ash_input_source* input = calloc(1u, sizeof(*input));
    if (input == NULL) {
        return NULL;
    }

    struct ash_source_name* name_record = shell->source_names;
    while (name_record != NULL &&
           strcmp(name_record->value, effective_name) != 0) {
        name_record = name_record->next;
    }
    if (name_record == NULL) {
        name_record = calloc(1u, sizeof(*name_record));
        if (name_record == NULL) {
            free(input);
            return NULL;
        }
        name_record->value = ash_input_duplicate_span(
            effective_name,
            strlen(effective_name)
        );
        if (name_record->value == NULL) {
            free(name_record);
            free(input);
            return NULL;
        }
    }

    struct ash_source_identity* identity =
        calloc(1u, sizeof(*identity));
    if (identity == NULL) {
        if (!name_record->published) {
            free(name_record->value);
            free(name_record);
        }
        free(input);
        return NULL;
    }
    identity->kind = kind;
    identity->name = name_record->value;
    input->kind = kind;
    input->transport = transport;
    input->name = name_record->value;
    input->name_record = name_record;
    input->identity = identity;
    return input;
}

static void ash_input_discard_candidate(struct ash_input_source* input) {
    if (input == NULL) {
        return;
    }
    if (!input->name_record->published) {
        free(input->name_record->value);
        free(input->name_record);
    }
    free(input->identity);
    free(input);
}

static bool ash_input_publish(
    struct ash_shell* shell,
    struct ash_input_source* input
) {
    struct ash_source_location caller =
        ash_execution_current_location(shell);
    if (ash_source_location_is_none(&caller) &&
        shell->input_stack != NULL) {
        const struct ash_input_source* active = shell->input_stack;
        caller = (struct ash_source_location){
            .source = active->name,
            .identity = active->identity,
            .line = active->logical_line != 0u ?
                active->logical_line :
                (active->physical_line != SIZE_MAX ?
                    active->physical_line + 1u :
                    active->physical_line),
            .column = 1u,
            .offset = active->parser_offset,
        };
    }
    if (caller.identity != NULL &&
        !ash_source_identity_retain(caller.identity)) {
        return false;
    }
    if (!input->name_record->published) {
        input->name_record->next = shell->source_names;
        input->name_record->published = true;
        shell->source_names = input->name_record;
    }
    input->identity->caller = caller;
    input->identity->reference_count = 1u;
    input->identity->next = shell->source_identities;
    input->identity->published = true;
    shell->source_identities = input->identity;
    ash_execution_push_source(
        shell,
        &input->execution_frame,
        input->kind,
        (struct ash_source_location){
            .source = input->name,
            .identity = input->identity,
            .line = 1u,
            .column = 1u,
        },
        caller
    );
    input->previous = shell->input_stack;
    shell->input_stack = input;
    return true;
}

bool ash_input_push_string_span(
    struct ash_shell* shell,
    enum ash_input_kind kind,
    const char* name,
    const char* text,
    size_t length
) {
    if (shell == NULL || text == NULL || !ash_input_kind_valid(kind)) {
        errno = EINVAL;
        return false;
    }
    assert(ash_input_stack_invariants(shell));
    assert(!shell->parser_state.active);

    struct ash_input_source* input = ash_input_create(
        shell,
        kind,
        ASH_INPUT_TRANSPORT_STRING,
        name
    );
    if (input == NULL) {
        return false;
    }

    input->source.string.text = ash_input_duplicate_span(text, length);
    if (input->source.string.text == NULL) {
        ash_input_discard_candidate(input);
        return false;
    }
    input->source.string.length = length;
    if (!ash_input_publish(shell, input)) {
        ash_input_discard_candidate(input);
        return false;
    }
    assert(ash_input_stack_invariants(shell));
    return true;
}

bool ash_input_push_file(
    struct ash_shell* shell,
    enum ash_input_kind kind,
    const char* name,
    FILE* stream,
    enum ash_input_stream_ownership ownership
) {
    if (shell == NULL || stream == NULL || !ash_input_kind_valid(kind) ||
        (ownership != ASH_INPUT_BORROW_STREAM &&
            ownership != ASH_INPUT_TAKE_STREAM)) {
        errno = EINVAL;
        return false;
    }
    assert(ash_input_stack_invariants(shell));
    assert(!shell->parser_state.active);

    struct ash_input_source* input = ash_input_create(
        shell,
        kind,
        ASH_INPUT_TRANSPORT_FILE,
        name
    );
    if (input == NULL) {
        return false;
    }

    input->source.file.stream = stream;
    input->source.file.ownership = ownership;
    if (!ash_input_publish(shell, input)) {
        ash_input_discard_candidate(input);
        return false;
    }
    assert(ash_input_stack_invariants(shell));
    return true;
}

void ash_input_pop(struct ash_shell* shell) {
    if (shell == NULL || shell->input_stack == NULL) {
        return;
    }
    assert(ash_input_stack_invariants(shell));
    assert(!shell->parser_state.active);

    struct ash_input_source* input = shell->input_stack;
    bool trace_popped = ash_execution_pop(
        shell,
        &input->execution_frame
    );
    assert(trace_popped);
    (void)trace_popped;
    shell->input_stack = input->previous;
    ash_source_identity_release(shell, input->identity);
    if (input->transport == ASH_INPUT_TRANSPORT_STRING) {
        free(input->source.string.text);
    }
    else if (input->source.file.ownership == ASH_INPUT_TAKE_STREAM) {
        fclose(input->source.file.stream);
    }
    free(input);
    assert(ash_input_stack_invariants(shell));
}

void ash_input_release_all(struct ash_shell* shell) {
    if (shell != NULL) {
        assert(ash_input_stack_invariants(shell));
        assert(!shell->parser_state.active);
    }
    while (shell != NULL && shell->input_stack != NULL) {
        ash_input_pop(shell);
    }
}

void ash_input_source_registry_destroy(struct ash_shell* shell) {
    if (shell != NULL) {
        assert(shell->input_stack == NULL);
        assert(shell->execution_frames == NULL);
        assert(shell->source_identities == NULL);
        assert(ash_input_stack_invariants(shell));
        assert(!shell->parser_state.active);
    }
    while (shell != NULL && shell->source_names != NULL) {
        struct ash_source_name* name = shell->source_names;
        shell->source_names = name->next;
        free(name->value);
        free(name);
    }
    if (shell != NULL) {
        assert(ash_input_stack_invariants(shell));
    }
}

static bool ash_input_reserve(char** line, size_t* capacity, size_t needed) {
    if (*capacity >= needed) {
        return true;
    }

    size_t grown = (*capacity == 0u) ? 128u : *capacity;
    while (grown < needed) {
        if (grown > SIZE_MAX / 2u) {
            grown = needed;
            break;
        }
        grown *= 2u;
    }

    char* replacement = realloc(*line, grown);
    if (replacement == NULL) {
        return false;
    }
    *line = replacement;
    *capacity = grown;
    return true;
}

static ssize_t ash_input_read_string(
    struct ash_input_source* input,
    char** line,
    size_t* capacity
) {
    size_t offset = input->byte_offset;
    size_t length = input->source.string.length;
    if (offset == length) {
        return -1;
    }

    const char* text = input->source.string.text;
    size_t end = offset;
    while (end < length && text[end] != '\n') {
        end++;
    }
    if (end < length) {
        end++;
    }

    size_t line_length = end - offset;
    if (line_length > (size_t)PTRDIFF_MAX || line_length == SIZE_MAX ||
        !ash_input_reserve(line, capacity, line_length + 1u)) {
        errno = ENOMEM;
        return -1;
    }

    memcpy(*line, text + offset, line_length);
    (*line)[line_length] = '\0';
    return (ssize_t)line_length;
}

ssize_t ash_input_read_line(struct ash_shell* shell, char** line, size_t* capacity) {
    if (shell == NULL || shell->input_stack == NULL || line == NULL || capacity == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct ash_input_source* input = shell->input_stack;
    assert(ash_input_stack_invariants(shell));
    ssize_t result;
    if (input->transport == ASH_INPUT_TRANSPORT_STRING) {
        result = ash_input_read_string(input, line, capacity);
    }
    else {
        result = getline(line, capacity, input->source.file.stream);
    }

    if (result >= 0) {
        size_t length = (size_t)result;
        if (input->physical_line == SIZE_MAX ||
            length > SIZE_MAX - input->byte_offset) {
            errno = EOVERFLOW;
            return -1;
        }
        input->physical_line++;
        input->byte_offset += length;
    }
    assert(ash_input_stack_invariants(shell));
    return result;
}

const char* ash_input_source_name(const struct ash_shell* shell) {
    if (shell == NULL || shell->input_stack == NULL) {
        return NULL;
    }
    return shell->input_stack->name;
}

enum ash_input_kind ash_input_source_kind(const struct ash_shell* shell) {
    if (shell == NULL || shell->input_stack == NULL) {
        return ASH_INPUT_INVALID;
    }
    return shell->input_stack->kind;
}

size_t ash_input_source_physical_line(const struct ash_shell* shell) {
    if (shell == NULL || shell->input_stack == NULL) {
        return 0u;
    }
    return shell->input_stack->physical_line;
}

size_t ash_input_source_logical_line(const struct ash_shell* shell) {
    if (shell == NULL || shell->input_stack == NULL) {
        return 0u;
    }
    return shell->input_stack->logical_line;
}

size_t ash_input_source_parser_offset(const struct ash_shell* shell) {
    if (shell == NULL || shell->input_stack == NULL) {
        return 0u;
    }
    return shell->input_stack->parser_offset;
}

struct ash_source_location ash_input_next_location(
    const struct ash_shell* shell
) {
    if (shell == NULL || shell->input_stack == NULL ||
        shell->input_stack->physical_line == SIZE_MAX) {
        return (struct ash_source_location){0};
    }
    return (struct ash_source_location){
        .source = shell->input_stack->name,
        .identity = shell->input_stack->identity,
        .line = shell->input_stack->physical_line + 1u,
        .column = 1u,
        .offset = shell->input_stack->byte_offset,
    };
}

bool ash_input_note_parse(
    struct ash_shell* shell,
    struct ash_source_location origin,
    size_t parser_offset
) {
    if (shell == NULL || shell->input_stack == NULL ||
        !ash_source_location_valid(&origin) ||
        origin.source != shell->input_stack->name ||
        origin.identity != shell->input_stack->identity ||
        origin.line > shell->input_stack->physical_line ||
        origin.offset > parser_offset ||
        parser_offset > shell->input_stack->byte_offset) {
        errno = EINVAL;
        return false;
    }
    shell->input_stack->logical_line = origin.line;
    shell->input_stack->parser_offset = parser_offset;
    assert(ash_input_stack_invariants(shell));
    return true;
}

bool ash_input_source_has_error(const struct ash_shell* shell) {
    return shell != NULL &&
        shell->input_stack != NULL &&
        shell->input_stack->transport == ASH_INPUT_TRANSPORT_FILE &&
        ferror(shell->input_stack->source.file.stream);
}
