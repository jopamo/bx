#include "applets/base/env/env_signal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/args_common.h"
#include "lib/cli_common.h"
#include "lib/output_quote.h"
#include "lib/signal_names.h"

static int bx_env_signal_bound(void) {
#if defined(SIGRTMAX)
    int bound = SIGRTMAX;
    return bound < BX_ENV_SIGNAL_STORAGE
        ? bound
        : BX_ENV_SIGNAL_STORAGE - 1;
#else
    return BX_ENV_SIGNAL_STORAGE - 1;
#endif
}

static int bx_env_signal_parse_operand(const char *operand) {
    int signal_number = -1;
    if (operand != NULL &&
        operand[0] >= '0' && operand[0] <= '9') {
        int parsed = 0;
        if (!bx_args_parse_int_range(operand, 0, INT_MAX, &parsed))
            return -1;
        signal_number = parsed;
        signal_number &= signal_number >= 0xff ? 0xff : 0x7f;
    } else if (!bx_signal_name_lookup(operand, &signal_number)) {
        return -1;
    }
    return signal_number > 0 && signal_number <= bx_env_signal_bound()
        ? signal_number
        : -1;
}

static bool bx_env_signal_each(
    const char *argument,
    bool (*apply)(int signal_number, void *user),
    void *user,
    struct bx_diag_ctx *diag) {
    if (argument == NULL)
        return true;

    char *copy = strdup(argument);
    if (copy == NULL) {
        bx_diag(diag, "memory exhausted");
        return false;
    }
    char *save = NULL;
    for (char *token = strtok_r(copy, ",", &save);
         token != NULL;
         token = strtok_r(NULL, ",", &save)) {
        int signal_number = bx_env_signal_parse_operand(token);
        if (signal_number < 0) {
            char *quoted = bx_output_quote_dup(
                token, BX_OUTPUT_QUOTE_LOCALE);
            bx_diag(diag, "%s: invalid signal", quoted);
            free(quoted);
            bx_cli_print_try_help(diag->progname);
            free(copy);
            return false;
        }
        if (!apply(signal_number, user)) {
            free(copy);
            return false;
        }
    }
    free(copy);
    return true;
}

void bx_env_signal_policy_init(struct bx_env_signal_policy *policy) {
    memset(policy, 0, sizeof(*policy));
    sigemptyset(&policy->block_signals);
    sigemptyset(&policy->unblock_signals);
}

struct bx_env_signal_action_update {
    struct bx_env_signal_policy *policy;
    enum bx_env_signal_mode mode;
};

static bool bx_env_signal_set_action(int signal_number, void *user) {
    struct bx_env_signal_action_update *update = user;
    update->policy->modes[signal_number] = update->mode;
    return true;
}

bool bx_env_signal_parse_action(
    struct bx_env_signal_policy *policy,
    const char *argument,
    bool set_default,
    struct bx_diag_ctx *diag) {
    if (argument == NULL) {
        enum bx_env_signal_mode mode = set_default
            ? BX_ENV_SIGNAL_DEFAULT_NOERR
            : BX_ENV_SIGNAL_IGNORE_NOERR;
        for (int signal_number = 1;
             signal_number <= bx_env_signal_bound();
             signal_number++) {
            policy->modes[signal_number] = mode;
        }
        return true;
    }

    struct bx_env_signal_action_update update = {
        .policy = policy,
        .mode = set_default
            ? BX_ENV_SIGNAL_DEFAULT
            : BX_ENV_SIGNAL_IGNORE,
    };
    return bx_env_signal_each(
        argument, bx_env_signal_set_action, &update, diag);
}

struct bx_env_signal_mask_update {
    struct bx_env_signal_policy *policy;
    bool block;
    struct bx_diag_ctx *diag;
};

static bool bx_env_signal_set_mask(int signal_number, void *user) {
    struct bx_env_signal_mask_update *update = user;
    sigset_t *set = update->block
        ? &update->policy->block_signals
        : &update->policy->unblock_signals;
    sigset_t *opposite = update->block
        ? &update->policy->unblock_signals
        : &update->policy->block_signals;
    if (sigaddset(set, signal_number) != 0) {
        if (update->block) {
            bx_diag(
                update->diag,
                "failed to block signal %d: %s",
                signal_number,
                strerror(errno));
            return false;
        }
        return true;
    }
    sigdelset(opposite, signal_number);
    return true;
}

bool bx_env_signal_parse_block(
    struct bx_env_signal_policy *policy,
    const char *argument,
    bool block,
    struct bx_diag_ctx *diag) {
    if (argument == NULL) {
        if (block) {
            sigfillset(&policy->block_signals);
            sigemptyset(&policy->unblock_signals);
        } else {
            sigfillset(&policy->unblock_signals);
            sigemptyset(&policy->block_signals);
        }
        policy->mask_changed = true;
        return true;
    }
    if (!policy->mask_changed) {
        sigemptyset(&policy->block_signals);
        sigemptyset(&policy->unblock_signals);
    }
    policy->mask_changed = true;

    struct bx_env_signal_mask_update update = {
        .policy = policy,
        .block = block,
        .diag = diag,
    };
    return bx_env_signal_each(
        argument, bx_env_signal_set_mask, &update, diag);
}

static void bx_env_signal_format(int signal_number,
                                 char *buffer,
                                 size_t buffer_size) {
    if (!bx_signal_name_format(signal_number, buffer, buffer_size))
        snprintf(buffer, buffer_size, "SIG%d", signal_number);
}

bool bx_env_signal_apply(
    const struct bx_env_signal_policy *policy,
    bool debug,
    struct bx_diag_ctx *diag) {
    for (int signal_number = 1;
         signal_number <= bx_env_signal_bound();
         signal_number++) {
        enum bx_env_signal_mode mode = policy->modes[signal_number];
        if (mode == BX_ENV_SIGNAL_UNCHANGED)
            continue;

        bool ignore_errors =
            mode == BX_ENV_SIGNAL_DEFAULT_NOERR ||
            mode == BX_ENV_SIGNAL_IGNORE_NOERR;
        bool set_default =
            mode == BX_ENV_SIGNAL_DEFAULT ||
            mode == BX_ENV_SIGNAL_DEFAULT_NOERR;
        struct sigaction action;
        int error = sigaction(signal_number, NULL, &action);
        if (error != 0 && !ignore_errors) {
            bx_diag(
                diag,
                "failed to get signal action for signal %d: %s",
                signal_number,
                strerror(errno));
            return false;
        }
        if (error == 0) {
            action.sa_handler = set_default ? SIG_DFL : SIG_IGN;
            error = sigaction(signal_number, &action, NULL);
            if (error != 0 && !ignore_errors) {
                bx_diag(
                    diag,
                    "failed to set signal action for signal %d: %s",
                    signal_number,
                    strerror(errno));
                return false;
            }
        }
        if (debug) {
            char name[32];
            bx_env_signal_format(signal_number, name, sizeof(name));
            fprintf(
                stderr,
                "Reset signal %s (%d) to %s%s\n",
                name,
                signal_number,
                set_default ? "DEFAULT" : "IGNORE",
                error != 0 ? " (failure ignored)" : "");
        }
    }

    if (!policy->mask_changed)
        return true;

    sigset_t mask;
    if (sigprocmask(0, NULL, &mask) != 0) {
        bx_diag(
            diag,
            "failed to get signal process mask: %s",
            strerror(errno));
        return false;
    }
    for (int signal_number = 1;
         signal_number <= bx_env_signal_bound();
         signal_number++) {
        const char *action = NULL;
        if (sigismember(&policy->block_signals, signal_number) == 1) {
            sigaddset(&mask, signal_number);
            action = "BLOCK";
        } else if (sigismember(
                       &policy->unblock_signals, signal_number) == 1) {
            sigdelset(&mask, signal_number);
            action = "UNBLOCK";
        }
        if (debug && action != NULL) {
            char name[32];
            bx_env_signal_format(signal_number, name, sizeof(name));
            fprintf(
                stderr,
                "signal %s (%d) mask set to %s\n",
                name,
                signal_number,
                action);
        }
    }
    if (sigprocmask(SIG_SETMASK, &mask, NULL) != 0) {
        bx_diag(
            diag,
            "failed to set signal process mask: %s",
            strerror(errno));
        return false;
    }
    return true;
}

bool bx_env_signal_list(struct bx_diag_ctx *diag) {
    sigset_t mask;
    if (sigprocmask(0, NULL, &mask) != 0) {
        bx_diag(
            diag,
            "failed to get signal process mask: %s",
            strerror(errno));
        return false;
    }

    for (int signal_number = 1;
         signal_number <= bx_env_signal_bound();
         signal_number++) {
        struct sigaction action;
        if (sigaction(signal_number, NULL, &action) != 0)
            continue;
        bool ignored = action.sa_handler == SIG_IGN;
        bool blocked = sigismember(&mask, signal_number) == 1;
        if (!ignored && !blocked)
            continue;

        char name[32];
        bx_env_signal_format(signal_number, name, sizeof(name));
        fprintf(
            stderr,
            "%-10s (%2d): %s%s%s\n",
            name,
            signal_number,
            blocked ? "BLOCK" : "",
            blocked && ignored ? "," : "",
            ignored ? "IGNORE" : "");
    }
    return true;
}
