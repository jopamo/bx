#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "applets/shell/ash/shell_context.h"
#include "applets/shell/ash/source_trace.h"

bool ash_input_kind_valid(enum ash_input_kind kind) {
    switch (kind) {
        case ASH_INPUT_INVALID:
            return false;
        case ASH_INPUT_COMMAND_STRING:
        case ASH_INPUT_STDIN:
        case ASH_INPUT_SCRIPT_FILE:
        case ASH_INPUT_SOURCED_FILE:
        case ASH_INPUT_EVAL:
        case ASH_INPUT_COMMAND_SUBSTITUTION:
        case ASH_INPUT_PROMPT_COMMAND:
        case ASH_INPUT_COMPLETION_HOOK:
        case ASH_INPUT_INTERACTIVE:
            return true;
    }
    return false;
}

static bool ash_execution_frame_chain_acyclic(
    const struct ash_execution_frame* frame
) {
    const struct ash_execution_frame* slow = frame;
    const struct ash_execution_frame* fast = frame;
    while (fast != NULL && fast->previous != NULL) {
        slow = slow->previous;
        fast = fast->previous->previous;
        if (slow == fast) {
            return false;
        }
    }
    return true;
}

bool ash_source_identity_is_owned(
    const struct ash_shell* shell,
    const struct ash_source_identity* identity
) {
    if (shell == NULL || identity == NULL) {
        return false;
    }
    for (const struct ash_source_identity* current =
             shell->source_identities;
         current != NULL;
         current = current->next) {
        if (current == identity) {
            return true;
        }
    }
    return false;
}

bool ash_execution_trace_invariants(const struct ash_shell* shell) {
    if (shell == NULL ||
        !ash_execution_frame_chain_acyclic(shell->execution_frames) ||
        (!ash_source_location_is_none(&shell->execution_location) &&
         (!ash_source_location_valid(&shell->execution_location) ||
          !ash_source_identity_is_owned(
              shell,
              shell->execution_location.identity
          ) ||
          !shell->execution_location.identity->published ||
          shell->execution_location.identity->reference_count == 0u ||
          shell->execution_location.source !=
              shell->execution_location.identity->name))) {
        return false;
    }

    for (const struct ash_execution_frame* frame = shell->execution_frames;
         frame != NULL;
         frame = frame->previous) {
        if (!ash_source_location_valid(&frame->entry) ||
            frame->entry.identity == NULL ||
            !ash_source_identity_is_owned(shell, frame->entry.identity) ||
            !frame->entry.identity->published ||
            frame->entry.identity->reference_count == 0u ||
            frame->entry.source != frame->entry.identity->name ||
            (frame->previous == NULL) !=
                ash_source_location_is_none(&frame->caller) ||
            (frame->previous != NULL &&
             (!ash_source_location_valid(&frame->caller) ||
              frame->caller.identity == NULL ||
              !ash_source_identity_is_owned(
                  shell,
                  frame->caller.identity
              ) ||
              !frame->caller.identity->published ||
              frame->caller.identity->reference_count == 0u ||
              frame->caller.source != frame->caller.identity->name))) {
            return false;
        }
        switch (frame->kind) {
            case ASH_EXECUTION_SOURCE_FRAME:
                if (!ash_input_kind_valid(frame->source_kind) ||
                    frame->function_name != NULL ||
                    frame->entry.identity->kind !=
                        frame->source_kind) {
                    return false;
                }
                break;
            case ASH_EXECUTION_FUNCTION_FRAME:
                if (frame->source_kind != ASH_INPUT_INVALID ||
                    frame->function_name == NULL ||
                    frame->function_name[0] == '\0' ||
                    frame->previous == NULL) {
                    return false;
                }
                break;
            default:
                return false;
        }
    }
    return true;
}

bool ash_source_identity_retain(struct ash_source_identity* identity) {
    if (identity == NULL || !identity->published ||
        identity->reference_count == 0u) {
        errno = EINVAL;
        return false;
    }
    if (identity->reference_count == SIZE_MAX) {
        errno = EOVERFLOW;
        return false;
    }
    identity->reference_count++;
    return true;
}

void ash_source_identity_release(
    struct ash_shell* shell,
    struct ash_source_identity* identity
) {
    assert(shell != NULL);
    while (identity != NULL) {
        assert(identity->published);
        assert(identity->reference_count != 0u);
        assert(ash_source_identity_is_owned(shell, identity));

        identity->reference_count--;
        if (identity->reference_count != 0u) {
            return;
        }

        struct ash_source_identity** link = &shell->source_identities;
        while (*link != NULL && *link != identity) {
            link = &(*link)->next;
        }
        assert(*link == identity);
        *link = identity->next;
        struct ash_source_identity* caller = identity->caller.identity;
        identity->published = false;
        free(identity);
        identity = caller;
    }
}

void ash_execution_push_source(
    struct ash_shell* shell,
    struct ash_execution_frame* frame,
    enum ash_input_kind source_kind,
    struct ash_source_location entry,
    struct ash_source_location caller
) {
    assert(shell != NULL);
    assert(frame != NULL);
    assert(ash_input_kind_valid(source_kind));
    assert(ash_source_location_valid(&entry));
    assert(entry.identity != NULL);
    assert(
        shell->execution_frames != NULL ?
            (ash_source_location_valid(&caller) &&
             caller.identity != NULL) :
            ash_source_location_is_none(&caller)
    );

    *frame = (struct ash_execution_frame){
        .kind = ASH_EXECUTION_SOURCE_FRAME,
        .source_kind = source_kind,
        .entry = entry,
        .caller = caller,
        .previous = shell->execution_frames,
    };
    shell->execution_frames = frame;
    assert(ash_execution_trace_invariants(shell));
}

bool ash_execution_push_function(
    struct ash_shell* shell,
    struct ash_execution_frame* frame,
    const char* function_name,
    struct ash_source_location definition,
    struct ash_source_location caller
) {
    assert(shell != NULL);
    assert(frame != NULL);
    assert(function_name != NULL && function_name[0] != '\0');
    assert(shell->execution_frames != NULL);
    assert(ash_source_location_valid(&definition));
    assert(definition.identity != NULL);
    assert(ash_source_location_valid(&caller));
    assert(caller.identity != NULL);
    if (!ash_source_identity_retain(definition.identity)) {
        return false;
    }

    *frame = (struct ash_execution_frame){
        .kind = ASH_EXECUTION_FUNCTION_FRAME,
        .source_kind = ASH_INPUT_INVALID,
        .function_name = function_name,
        .entry = definition,
        .caller = caller,
        .previous = shell->execution_frames,
    };
    shell->execution_frames = frame;
    assert(ash_execution_trace_invariants(shell));
    return true;
}

bool ash_execution_pop(
    struct ash_shell* shell,
    struct ash_execution_frame* frame
) {
    if (shell == NULL || frame == NULL ||
        shell->execution_frames != frame) {
        errno = EINVAL;
        return false;
    }
    assert(ash_execution_trace_invariants(shell));
    struct ash_source_identity* retained_definition =
        frame->kind == ASH_EXECUTION_FUNCTION_FRAME ?
            frame->entry.identity :
            NULL;
    shell->execution_frames = frame->previous;
    *frame = (struct ash_execution_frame){0};
    if (retained_definition != NULL) {
        ash_source_identity_release(shell, retained_definition);
    }
    assert(ash_execution_trace_invariants(shell));
    return true;
}

struct ash_source_location ash_execution_current_location(
    const struct ash_shell* shell
) {
    if (shell == NULL) {
        return (struct ash_source_location){0};
    }
    return shell->execution_location;
}

void ash_execution_location_enter(
    struct ash_shell* shell,
    struct ash_source_location location,
    struct ash_execution_location_guard* guard
) {
    assert(shell != NULL);
    assert(guard != NULL && !guard->active);
    assert(ash_source_location_valid(&location));
    assert(location.identity != NULL);
    assert(ash_execution_trace_invariants(shell));

    guard->previous = shell->execution_location;
    guard->active = true;
    shell->execution_location = location;
    assert(ash_execution_trace_invariants(shell));
}

void ash_execution_location_leave(
    struct ash_shell* shell,
    struct ash_execution_location_guard* guard
) {
    assert(shell != NULL);
    assert(guard != NULL && guard->active);
    assert(ash_execution_trace_invariants(shell));

    shell->execution_location = guard->previous;
    *guard = (struct ash_execution_location_guard){0};
    assert(ash_execution_trace_invariants(shell));
}
