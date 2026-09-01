#ifndef BX_APPLETS_SHELL_ASH_STARTUP_H
#define BX_APPLETS_SHELL_ASH_STARTUP_H

#include <stdbool.h>
#include <stddef.h>

struct ash_shell;

enum ash_bashrc_selection {
    ASH_BASHRC_DEFAULT = 0,
    ASH_BASHRC_SUPPRESSED,
    ASH_BASHRC_EXPLICIT,
};

enum ash_profile_selection {
    ASH_PROFILES_DEFAULT = 0,
    ASH_PROFILES_SUPPRESSED,
};

/*
 * The path borrows invocation argv storage. Bashrc suppression is
 * authoritative: once requested, a later --rcfile cannot silently reactivate
 * startup input. Profile suppression is independent of non-login bashrc
 * selection.
 */
struct ash_startup_request {
    enum ash_bashrc_selection bashrc;
    const char* bashrc_path;
    enum ash_profile_selection profiles;
};

enum ash_startup_outcome {
    ASH_STARTUP_CONTINUE = 0,
    ASH_STARTUP_EXIT,
    ASH_STARTUP_FATAL,
};

static inline bool ash_startup_request_valid(
    const struct ash_startup_request* request
) {
    if (request == NULL) {
        return false;
    }
    if (request->profiles != ASH_PROFILES_DEFAULT &&
        request->profiles != ASH_PROFILES_SUPPRESSED) {
        return false;
    }
    switch (request->bashrc) {
        case ASH_BASHRC_DEFAULT:
        case ASH_BASHRC_SUPPRESSED:
            return request->bashrc_path == NULL;
        case ASH_BASHRC_EXPLICIT:
            return request->bashrc_path != NULL;
    }
    return false;
}

enum ash_startup_outcome ash_startup_execute(
    struct ash_shell* shell,
    const struct ash_startup_request* request
);

#endif /* BX_APPLETS_SHELL_ASH_STARTUP_H */
