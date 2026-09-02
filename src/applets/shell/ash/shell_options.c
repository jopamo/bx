#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "applets/shell/ash/shell_options.h"

enum ash_shell_option_state_source {
    ASH_OPTION_STATE_STORED = 0,
    ASH_OPTION_STATE_INTERACTIVE,
    ASH_OPTION_STATE_PRIVILEGED,
};

struct ash_shell_option_descriptor {
    const char* name;
    uint32_t option;
    uint32_t uses;
    char letter;
    enum ash_shell_option_state_source state;
    uint32_t personalities;
};

#define ASH_OPTION_INVOCATION_AND_SET \
    (ASH_SHELL_OPTION_USE_INVOCATION_SHORT | \
     ASH_SHELL_OPTION_USE_INVOCATION_NAME | \
     ASH_SHELL_OPTION_USE_SET_SHORT | \
     ASH_SHELL_OPTION_USE_SET_NAME)
#define ASH_OPTION_PERSONALITY_BASH \
    (1u << ASH_SHELL_PERSONALITY_BASH)
#define ASH_OPTION_PERSONALITY_ALL \
    ((1u << ASH_SHELL_PERSONALITY_POSIX_SH) | \
     ASH_OPTION_PERSONALITY_BASH)

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
        ASH_OPTION_STATE_STORED,
        ASH_OPTION_PERSONALITY_ALL,
    },
    {
        "notify",
        ASH_SHELL_OPTION_NOTIFY,
        0u,
        'b',
        ASH_OPTION_STATE_STORED,
        ASH_OPTION_PERSONALITY_ALL,
    },
    {
        "noclobber",
        ASH_SHELL_OPTION_NOCLOBBER,
        ASH_OPTION_INVOCATION_AND_SET,
        'C',
        ASH_OPTION_STATE_STORED,
        ASH_OPTION_PERSONALITY_ALL,
    },
    {
        "errexit",
        ASH_SHELL_OPTION_ERREXIT,
        0u,
        'e',
        ASH_OPTION_STATE_STORED,
        ASH_OPTION_PERSONALITY_ALL,
    },
    {
        "noglob",
        ASH_SHELL_OPTION_NOGLOB,
        ASH_OPTION_INVOCATION_AND_SET,
        'f',
        ASH_OPTION_STATE_STORED,
        ASH_OPTION_PERSONALITY_ALL,
    },
    {
        NULL,
        0u,
        0u,
        'i',
        ASH_OPTION_STATE_INTERACTIVE,
        ASH_OPTION_PERSONALITY_ALL,
    },
    {
        "monitor",
        ASH_SHELL_OPTION_MONITOR,
        0u,
        'm',
        ASH_OPTION_STATE_STORED,
        ASH_OPTION_PERSONALITY_ALL,
    },
    {
        "noexec",
        ASH_SHELL_OPTION_NOEXEC,
        0u,
        'n',
        ASH_OPTION_STATE_STORED,
        ASH_OPTION_PERSONALITY_ALL,
    },
    {
        "privileged",
        0u,
        0u,
        'p',
        ASH_OPTION_STATE_PRIVILEGED,
        ASH_OPTION_PERSONALITY_ALL,
    },
    {
        NULL,
        ASH_SHELL_OPTION_STDIN,
        ASH_SHELL_OPTION_USE_INVOCATION_SHORT,
        's',
        ASH_OPTION_STATE_STORED,
        ASH_OPTION_PERSONALITY_ALL,
    },
    {
        "onecmd",
        ASH_SHELL_OPTION_ONECMD,
        ASH_OPTION_INVOCATION_AND_SET,
        't',
        ASH_OPTION_STATE_STORED,
        ASH_OPTION_PERSONALITY_BASH,
    },
    {
        "nounset",
        ASH_SHELL_OPTION_NOUNSET,
        0u,
        'u',
        ASH_OPTION_STATE_STORED,
        ASH_OPTION_PERSONALITY_ALL,
    },
    {
        "verbose",
        ASH_SHELL_OPTION_VERBOSE,
        ASH_OPTION_INVOCATION_AND_SET,
        'v',
        ASH_OPTION_STATE_STORED,
        ASH_OPTION_PERSONALITY_ALL,
    },
    {
        "xtrace",
        ASH_SHELL_OPTION_XTRACE,
        0u,
        'x',
        ASH_OPTION_STATE_STORED,
        ASH_OPTION_PERSONALITY_ALL,
    },
};

#undef ASH_OPTION_PERSONALITY_ALL
#undef ASH_OPTION_PERSONALITY_BASH
#undef ASH_OPTION_INVOCATION_AND_SET

static bool ash_shell_option_use_valid(
    enum ash_shell_option_use use
) {
    return use == ASH_SHELL_OPTION_USE_INVOCATION_SHORT ||
        use == ASH_SHELL_OPTION_USE_INVOCATION_NAME ||
        use == ASH_SHELL_OPTION_USE_SET_SHORT ||
        use == ASH_SHELL_OPTION_USE_SET_NAME;
}

static uint32_t ash_shell_option_personality_mask(
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

bool ash_shell_options_valid(uint32_t options) {
    return (options & ~ASH_SHELL_OPTION_ALL) == 0u;
}

bool ash_shell_options_valid_for_personality(
    uint32_t options,
    enum ash_shell_personality personality
) {
    uint32_t personality_mask =
        ash_shell_option_personality_mask(personality);
    if (!ash_shell_options_valid(options) ||
        personality_mask == 0u) {
        return false;
    }
    for (size_t i = 0u;
         i < sizeof(ash_shell_options) /
             sizeof(ash_shell_options[0]);
         i++) {
        if ((ash_shell_options[i].personalities &
             personality_mask) == 0u &&
            (options & ash_shell_options[i].option) != 0u) {
            return false;
        }
    }
    return true;
}

static enum ash_shell_option_result ash_shell_option_apply(
    uint32_t* options,
    const struct ash_shell_option_descriptor* descriptor,
    bool enabled,
    enum ash_shell_personality personality,
    enum ash_shell_option_use use
) {
    if (options == NULL || descriptor == NULL ||
        !ash_shell_options_valid_for_personality(
            *options,
            personality
        ) ||
        !ash_shell_option_use_valid(use)) {
        return ASH_SHELL_OPTION_UNKNOWN;
    }
    if ((descriptor->uses & (uint32_t)use) == 0u ||
        (descriptor->personalities &
         ash_shell_option_personality_mask(personality)) == 0u) {
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
    enum ash_shell_personality personality,
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
                personality,
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
    enum ash_shell_personality personality,
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
                personality,
                use
            );
        }
    }
    return ASH_SHELL_OPTION_UNKNOWN;
}

void ash_shell_options_format_letters(
    uint32_t options,
    bool interactive,
    bool privileged,
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
        bool enabled = false;
        switch (ash_shell_options[i].state) {
            case ASH_OPTION_STATE_STORED:
                enabled =
                    (options & ash_shell_options[i].option) != 0u;
                break;
            case ASH_OPTION_STATE_INTERACTIVE:
                enabled = interactive;
                break;
            case ASH_OPTION_STATE_PRIVILEGED:
                enabled = privileged;
                break;
        }
        if (enabled && length + 1u < output_size) {
            output[length++] = ash_shell_options[i].letter;
        }
    }
    output[length] = '\0';
}
