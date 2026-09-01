#ifndef BX_APPLETS_SHELL_ASH_INPUT_EXECUTION_H
#define BX_APPLETS_SHELL_ASH_INPUT_EXECUTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "applets/shell/ash/input.h"

struct ash_shell;

const char* ash_input_default_prompt(void);

/*
 * Execute one source as the active top of the context-owned input stack.
 * String input is copied before publication. A TAKE stream is consumed even
 * when publication fails; a BORROW stream always remains caller-owned.
 */
int ash_input_execute_string(
    struct ash_shell* shell,
    enum ash_input_kind kind,
    const char* name,
    const char* text,
    size_t length
);
int ash_input_execute_stream(
    struct ash_shell* shell,
    enum ash_input_kind kind,
    const char* name,
    FILE* stream,
    enum ash_input_stream_ownership ownership,
    bool prompt
);

/*
 * Prompt and completion command strings share this single hook ingress.
 * Unknown source kinds fail closed instead of silently becoming eval input.
 */
int ash_input_execute_hook(
    struct ash_shell* shell,
    enum ash_input_kind kind,
    const char* command
);

#endif /* BX_APPLETS_SHELL_ASH_INPUT_EXECUTION_H */
