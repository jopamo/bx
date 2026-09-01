#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "applets/shell/ash/shell_options.h"

struct ash_shell_option_descriptor {
    const char* name;
    uint32_t option;
    uint32_t uses;
    char letter;
    bool interactive;
};

#define ASH_OPTION_INVOCATION_AND_SET \
    (ASH_SHELL_OPTION_USE_INVOCATION_SHORT | \
     ASH_SHELL_OPTION_USE_INVOCATION_NAME | \
     ASH_SHELL_OPTION_USE_SET_SHORT | \
     ASH_SHELL_OPTION_USE_SET_NAME)

/*
 * This is the sole spelling-to-state catalog. Options with no allowed uses
 * reserve their canonical representation for future semantics but cannot be
 * enabled through a user boundary.
 */
static const struct ash_shell_option_descriptor ash_shell_options[] = {
    {
        "allexport",
        ASH_SHELL_OPTION_ALLEXPORT,
        ASH_OPTION_INVOCATION_AND_SET,
        'a',
        false,
    },
    {"notify", ASH_SHELL_OPTION_NOTIFY, 0u, 'b', false},
    {
        "noclobber",
        ASH_SHELL_OPTION_NOCLOBBER,
        ASH_OPTION_INVOCATION_AND_SET,
        'C',
        false,
    },
    {"errexit", ASH_SHELL_OPTION_ERREXIT, 0u, 'e', false},
    {"noglob", ASH_SHELL_OPTION_NOGLOB, 0u, 'f', false},
    {NULL, 0u, 0u, 'i', true},
    {"monitor", ASH_SHELL_OPTION_MONITOR, 0u, 'm', false},
    {"noexec", ASH_SHELL_OPTION_NOEXEC, 0u, 'n', false},
    {
        NULL,
        ASH_SHELL_OPTION_STDIN,
        ASH_SHELL_OPTION_USE_INVOCATION_SHORT,
        's',
        false,
    },
    {"nounset", ASH_SHELL_OPTION_NOUNSET, 0u, 'u', false},
    {
        "verbose",
        ASH_SHELL_OPTION_VERBOSE,
        ASH_OPTION_INVOCATION_AND_SET,
        'v',
        false,
    },
    {"xtrace", ASH_SHELL_OPTION_XTRACE, 0u, 'x', false},
};

#undef ASH_OPTION_INVOCATION_AND_SET

static bool ash_shell_option_use_valid(
    enum ash_shell_option_use use
) {
    return use == ASH_SHELL_OPTION_USE_INVOCATION_SHORT ||
        use == ASH_SHELL_OPTION_USE_INVOCATION_NAME ||
        use == ASH_SHELL_OPTION_USE_SET_SHORT ||
        use == ASH_SHELL_OPTION_USE_SET_NAME;
}

bool ash_shell_options_valid(uint32_t options) {
    return (options & ~ASH_SHELL_OPTION_ALL) == 0u;
}

static enum ash_shell_option_result ash_shell_option_apply(
    uint32_t* options,
    const struct ash_shell_option_descriptor* descriptor,
    bool enabled,
    enum ash_shell_option_use use
) {
    if (options == NULL || descriptor == NULL ||
        !ash_shell_options_valid(*options) ||
        !ash_shell_option_use_valid(use)) {
        return ASH_SHELL_OPTION_UNKNOWN;
    }
    if ((descriptor->uses & (uint32_t)use) == 0u) {
        return ASH_SHELL_OPTION_UNAVAILABLE;
    }
    if (enabled) {
        *options |= descriptor->option;
    }
    else {
        *options &= ~descriptor->option;
    }
    return ASH_SHELL_OPTION_APPLIED;
}

enum ash_shell_option_result ash_shell_option_apply_letter(
    uint32_t* options,
    char letter,
    bool enabled,
    enum ash_shell_option_use use
) {
    for (size_t i = 0u;
         i < sizeof(ash_shell_options) /
             sizeof(ash_shell_options[0]);
         i++) {
        if (ash_shell_options[i].letter == letter) {
            return ash_shell_option_apply(
                options,
                &ash_shell_options[i],
                enabled,
                use
            );
        }
    }
    return ASH_SHELL_OPTION_UNKNOWN;
}

enum ash_shell_option_result ash_shell_option_apply_name(
    uint32_t* options,
    const char* name,
    bool enabled,
    enum ash_shell_option_use use
) {
    if (name == NULL) {
        return ASH_SHELL_OPTION_UNKNOWN;
    }
    for (size_t i = 0u;
         i < sizeof(ash_shell_options) /
             sizeof(ash_shell_options[0]);
         i++) {
        if (ash_shell_options[i].name != NULL &&
            strcmp(ash_shell_options[i].name, name) == 0) {
            return ash_shell_option_apply(
                options,
                &ash_shell_options[i],
                enabled,
                use
            );
        }
    }
    return ASH_SHELL_OPTION_UNKNOWN;
}

void ash_shell_options_format_letters(
    uint32_t options,
    bool interactive,
    char* output,
    size_t output_size
) {
    if (output == NULL || output_size == 0u) {
        return;
    }

    size_t length = 0u;
    for (size_t i = 0u;
         i < sizeof(ash_shell_options) /
             sizeof(ash_shell_options[0]);
         i++) {
        bool enabled = ash_shell_options[i].interactive ?
            interactive :
            (options & ash_shell_options[i].option) != 0u;
        if (enabled && length + 1u < output_size) {
            output[length++] = ash_shell_options[i].letter;
        }
    }
    output[length] = '\0';
}
