#include <stddef.h>
#include <string.h>

#include "applets/shell/ash/shopt.h"

struct ash_shopt_descriptor {
    const char* name;
    enum ash_shopt_option option;
    uint32_t personalities;
};

#define ASH_SHOPT_PERSONALITY_BASH \
    (1u << ASH_SHELL_PERSONALITY_BASH)

static const struct ash_shopt_descriptor ash_shopt_options[] = {
    {"extglob", ASH_SHOPT_EXTGLOB, ASH_SHOPT_PERSONALITY_BASH},
};

#undef ASH_SHOPT_PERSONALITY_BASH

static uint32_t ash_shopt_personality_mask(
    enum ash_shell_personality personality
) {
    switch (personality) {
        case ASH_SHELL_PERSONALITY_INVALID:
            return 0u;
        case ASH_SHELL_PERSONALITY_POSIX_SH:
        case ASH_SHELL_PERSONALITY_BASH:
            return 1u << personality;
    }
    return 0u;
}

static const struct ash_shopt_descriptor* ash_shopt_find(
    enum ash_shopt_option option
) {
    for (size_t i = 0u;
         i < sizeof(ash_shopt_options) /
             sizeof(ash_shopt_options[0]);
         i++) {
        if (ash_shopt_options[i].option == option) {
            return &ash_shopt_options[i];
        }
    }
    return NULL;
}

enum ash_shopt_option ash_shopt_resolve(const char* name) {
    if (name == NULL) {
        return 0u;
    }
    for (size_t i = 0u;
         i < sizeof(ash_shopt_options) /
             sizeof(ash_shopt_options[0]);
         i++) {
        if (strcmp(ash_shopt_options[i].name, name) == 0) {
            return ash_shopt_options[i].option;
        }
    }
    return 0u;
}

const char* ash_shopt_name(enum ash_shopt_option option) {
    const struct ash_shopt_descriptor* descriptor =
        ash_shopt_find(option);
    return descriptor != NULL ? descriptor->name : NULL;
}

bool ash_shopt_state_valid(const struct ash_shopt_state* state) {
    return state != NULL &&
        (state->enabled & ~(uint32_t)ASH_SHOPT_ALL) == 0u;
}

bool ash_shopt_state_valid_for_personality(
    const struct ash_shopt_state* state,
    enum ash_shell_personality personality
) {
    uint32_t personality_mask =
        ash_shopt_personality_mask(personality);
    if (!ash_shopt_state_valid(state) || personality_mask == 0u) {
        return false;
    }
    for (size_t i = 0u;
         i < sizeof(ash_shopt_options) /
             sizeof(ash_shopt_options[0]);
         i++) {
        if ((state->enabled & ash_shopt_options[i].option) != 0u &&
            (ash_shopt_options[i].personalities &
             personality_mask) == 0u) {
            return false;
        }
    }
    return true;
}

enum ash_shopt_result ash_shopt_apply(
    struct ash_shopt_state* state,
    enum ash_shopt_option option,
    bool enabled,
    enum ash_shell_personality personality
) {
    const struct ash_shopt_descriptor* descriptor =
        ash_shopt_find(option);
    uint32_t personality_mask =
        ash_shopt_personality_mask(personality);
    if (!ash_shopt_state_valid_for_personality(
            state,
            personality
        )) {
        return ASH_SHOPT_UNKNOWN;
    }
    if (descriptor == NULL) {
        return ASH_SHOPT_UNKNOWN;
    }
    if ((descriptor->personalities & personality_mask) == 0u) {
        return ASH_SHOPT_UNAVAILABLE;
    }
    if (enabled) {
        state->enabled |= descriptor->option;
    }
    else {
        state->enabled &= ~descriptor->option;
    }
    return ASH_SHOPT_APPLIED;
}

bool ash_shopt_enabled(
    const struct ash_shopt_state* state,
    enum ash_shopt_option option
) {
    return ash_shopt_state_valid(state) &&
        (option & ~(uint32_t)ASH_SHOPT_ALL) == 0u &&
        option != 0u &&
        (state->enabled & (uint32_t)option) == (uint32_t)option;
}
