#ifndef BX_APPLETS_SHELL_ASH_SHOPT_H
#define BX_APPLETS_SHELL_ASH_SHOPT_H

#include <stdbool.h>
#include <stdint.h>

#include "applets/shell/ash/shell_policy.h"

enum ash_shopt_option {
    ASH_SHOPT_EXTGLOB = 1u << 0,
    ASH_SHOPT_ALL = ASH_SHOPT_EXTGLOB,
};

enum ash_shopt_result {
    ASH_SHOPT_UNKNOWN = 0,
    ASH_SHOPT_UNAVAILABLE,
    ASH_SHOPT_APPLIED,
};

struct ash_shopt_state {
    uint32_t enabled;
};

bool ash_shopt_state_valid(const struct ash_shopt_state* state);
bool ash_shopt_state_valid_for_personality(
    const struct ash_shopt_state* state,
    enum ash_shell_personality personality
);
enum ash_shopt_option ash_shopt_resolve(const char* name);
const char* ash_shopt_name(enum ash_shopt_option option);
enum ash_shopt_result ash_shopt_apply(
    struct ash_shopt_state* state,
    enum ash_shopt_option option,
    bool enabled,
    enum ash_shell_personality personality
);
bool ash_shopt_enabled(
    const struct ash_shopt_state* state,
    enum ash_shopt_option option
);

#endif /* BX_APPLETS_SHELL_ASH_SHOPT_H */
