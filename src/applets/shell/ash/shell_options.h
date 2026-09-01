#ifndef BX_APPLETS_SHELL_ASH_SHELL_OPTIONS_H
#define BX_APPLETS_SHELL_ASH_SHELL_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "applets/shell/ash/shell_policy.h"

enum ash_shell_option {
    ASH_SHELL_OPTION_ALLEXPORT = 1u << 0,
    ASH_SHELL_OPTION_NOTIFY = 1u << 1,
    ASH_SHELL_OPTION_NOCLOBBER = 1u << 2,
    ASH_SHELL_OPTION_ERREXIT = 1u << 3,
    ASH_SHELL_OPTION_NOGLOB = 1u << 4,
    ASH_SHELL_OPTION_MONITOR = 1u << 5,
    ASH_SHELL_OPTION_NOEXEC = 1u << 6,
    ASH_SHELL_OPTION_NOUNSET = 1u << 7,
    ASH_SHELL_OPTION_VERBOSE = 1u << 8,
    ASH_SHELL_OPTION_XTRACE = 1u << 9,
    ASH_SHELL_OPTION_STDIN = 1u << 10,
    ASH_SHELL_OPTION_ONECMD = 1u << 11,
    ASH_SHELL_OPTION_ALL = (1u << 12) - 1u,
};

enum ash_shell_option_use {
    ASH_SHELL_OPTION_USE_INVOCATION_SHORT = 1u << 0,
    ASH_SHELL_OPTION_USE_INVOCATION_NAME = 1u << 1,
    ASH_SHELL_OPTION_USE_SET_SHORT = 1u << 2,
    ASH_SHELL_OPTION_USE_SET_NAME = 1u << 3,
};

enum ash_shell_option_result {
    ASH_SHELL_OPTION_UNKNOWN = 0,
    ASH_SHELL_OPTION_UNAVAILABLE,
    ASH_SHELL_OPTION_APPLIED,
};

bool ash_shell_options_valid(uint32_t options);
bool ash_shell_options_valid_for_personality(
    uint32_t options,
    enum ash_shell_personality personality
);
enum ash_shell_option_result ash_shell_option_apply_letter(
    uint32_t* options,
    char letter,
    bool enabled,
    enum ash_shell_personality personality,
    enum ash_shell_option_use use
);
enum ash_shell_option_result ash_shell_option_apply_name(
    uint32_t* options,
    const char* name,
    bool enabled,
    enum ash_shell_personality personality,
    enum ash_shell_option_use use
);
void ash_shell_options_format_letters(
    uint32_t options,
    bool interactive,
    char* output,
    size_t output_size
);

#endif /* BX_APPLETS_SHELL_ASH_SHELL_OPTIONS_H */
