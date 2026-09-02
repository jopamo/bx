#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "applets/shell/ash/shell_policy.h"

static const char ash_bash_baseline_version[] = "5.3.15(1)-release";

static bool ash_bash_compat_valid(enum ash_bash_compat_level compatibility) {
    switch (compatibility) {
        case ASH_BASH_COMPAT_31:
        case ASH_BASH_COMPAT_32:
        case ASH_BASH_COMPAT_40:
        case ASH_BASH_COMPAT_41:
        case ASH_BASH_COMPAT_42:
        case ASH_BASH_COMPAT_43:
        case ASH_BASH_COMPAT_44:
        case ASH_BASH_COMPAT_50:
        case ASH_BASH_COMPAT_51:
        case ASH_BASH_COMPAT_52:
        case ASH_BASH_COMPAT_53:
            return true;
        case ASH_BASH_COMPAT_NONE:
            return false;
    }
    return false;
}

struct ash_shell_policy ash_shell_policy_posix_sh(uint32_t flags) {
    return (struct ash_shell_policy){
        .personality = ASH_SHELL_PERSONALITY_POSIX_SH,
        .flags = flags | ASH_SHELL_POLICY_POSIX,
        .bash_compat = ASH_BASH_COMPAT_NONE,
    };
}

struct ash_shell_policy ash_shell_policy_bash(
    enum ash_bash_compat_level compatibility,
    uint32_t flags
) {
    return (struct ash_shell_policy){
        .personality = ASH_SHELL_PERSONALITY_BASH,
        .flags = flags,
        .bash_compat = compatibility,
    };
}

bool ash_shell_policy_for_invocation(
    const char* name,
    uint32_t flags,
    struct ash_shell_policy* policy
) {
    if (name == NULL || policy == NULL) {
        return false;
    }
    if (strcmp(name, "bash") == 0) {
        *policy = ash_shell_policy_bash(ASH_BASH_COMPAT_53, flags);
        return true;
    }
    if (strcmp(name, "ash") == 0 || strcmp(name, "sh") == 0) {
        *policy = ash_shell_policy_posix_sh(flags);
        return true;
    }
    return false;
}

bool ash_shell_policy_valid(const struct ash_shell_policy* policy) {
    const uint32_t known_flags = ASH_SHELL_POLICY_POSIX |
        ASH_SHELL_POLICY_INTERACTIVE |
        ASH_SHELL_POLICY_LOGIN |
        ASH_SHELL_POLICY_RESTRICTED |
        ASH_SHELL_POLICY_PRIVILEGED |
        ASH_SHELL_POLICY_STANDALONE_APPLETS |
        ASH_SHELL_POLICY_STARTUP_SUPPRESSED;

    if (policy == NULL || (policy->flags & ~known_flags) != 0u) {
        return false;
    }
    switch (policy->personality) {
        case ASH_SHELL_PERSONALITY_INVALID:
            return false;
        case ASH_SHELL_PERSONALITY_POSIX_SH:
            return (policy->flags & ASH_SHELL_POLICY_POSIX) != 0u &&
                policy->bash_compat == ASH_BASH_COMPAT_NONE;
        case ASH_SHELL_PERSONALITY_BASH:
            return ash_bash_compat_valid(policy->bash_compat);
    }
    return false;
}

bool ash_shell_policy_has(
    const struct ash_shell_policy* policy,
    enum ash_shell_policy_flag flag
) {
    return policy != NULL && (policy->flags & (uint32_t)flag) != 0u;
}

bool ash_shell_policy_is_bash(const struct ash_shell_policy* policy) {
    return policy != NULL &&
        policy->personality == ASH_SHELL_PERSONALITY_BASH;
}

bool ash_shell_policy_expands_aliases(
    const struct ash_shell_policy* policy
) {
    return ash_shell_policy_valid(policy) &&
        (ash_shell_policy_has(policy, ASH_SHELL_POLICY_POSIX) ||
         ash_shell_policy_has(policy, ASH_SHELL_POLICY_INTERACTIVE));
}

bool ash_shell_policy_allows_startup(
    const struct ash_shell_policy* policy
) {
    return ash_shell_policy_valid(policy) &&
        !ash_shell_policy_has(
            policy,
            ASH_SHELL_POLICY_STARTUP_SUPPRESSED
        );
}

const char* ash_shell_policy_bash_version(
    const struct ash_shell_policy* policy
) {
    if (!ash_shell_policy_valid(policy) ||
        !ash_shell_policy_is_bash(policy)) {
        return NULL;
    }
    return ash_bash_baseline_version;
}
