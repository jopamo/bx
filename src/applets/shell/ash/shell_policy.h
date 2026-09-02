#ifndef BX_APPLETS_SHELL_ASH_SHELL_POLICY_H
#define BX_APPLETS_SHELL_ASH_SHELL_POLICY_H

#include <stdbool.h>
#include <stdint.h>

enum ash_shell_personality {
    ASH_SHELL_PERSONALITY_INVALID = -1,
    ASH_SHELL_PERSONALITY_POSIX_SH = 0,
    ASH_SHELL_PERSONALITY_BASH,
};

enum ash_shell_policy_flag {
    ASH_SHELL_POLICY_POSIX = 1u << 0,
    ASH_SHELL_POLICY_INTERACTIVE = 1u << 1,
    ASH_SHELL_POLICY_LOGIN = 1u << 2,
    ASH_SHELL_POLICY_RESTRICTED = 1u << 3,
    ASH_SHELL_POLICY_PRIVILEGED = 1u << 4,
    ASH_SHELL_POLICY_STANDALONE_APPLETS = 1u << 5,
    ASH_SHELL_POLICY_STARTUP_SUPPRESSED = 1u << 6,
};

enum ash_bash_compat_level {
    ASH_BASH_COMPAT_NONE = 0,
    ASH_BASH_COMPAT_31 = 31,
    ASH_BASH_COMPAT_32 = 32,
    ASH_BASH_COMPAT_40 = 40,
    ASH_BASH_COMPAT_41 = 41,
    ASH_BASH_COMPAT_42 = 42,
    ASH_BASH_COMPAT_43 = 43,
    ASH_BASH_COMPAT_44 = 44,
    ASH_BASH_COMPAT_50 = 50,
    ASH_BASH_COMPAT_51 = 51,
    ASH_BASH_COMPAT_52 = 52,
    ASH_BASH_COMPAT_53 = 53,
};

struct ash_shell_policy {
    enum ash_shell_personality personality;
    uint32_t flags;
    enum ash_bash_compat_level bash_compat;
};

struct ash_shell_policy ash_shell_policy_posix_sh(uint32_t flags);
struct ash_shell_policy ash_shell_policy_bash(
    enum ash_bash_compat_level compatibility,
    uint32_t flags
);
bool ash_shell_policy_for_invocation(
    const char* name,
    uint32_t flags,
    struct ash_shell_policy* policy
);
bool ash_shell_policy_valid(const struct ash_shell_policy* policy);
bool ash_shell_policy_has(
    const struct ash_shell_policy* policy,
    enum ash_shell_policy_flag flag
);
bool ash_shell_policy_is_bash(const struct ash_shell_policy* policy);
bool ash_shell_policy_expands_aliases(
    const struct ash_shell_policy* policy
);
bool ash_shell_policy_allows_startup(
    const struct ash_shell_policy* policy
);
const char* ash_shell_policy_bash_version(
    const struct ash_shell_policy* policy
);

#endif /* BX_APPLETS_SHELL_ASH_SHELL_POLICY_H */
