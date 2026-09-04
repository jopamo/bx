#ifndef BX_APPLETS_BASE_ENV_SIGNAL_H
#define BX_APPLETS_BASE_ENV_SIGNAL_H

#include <stdbool.h>
#include <signal.h>

#include "bx/diag.h"

enum bx_env_signal_mode {
    BX_ENV_SIGNAL_UNCHANGED = 0,
    BX_ENV_SIGNAL_DEFAULT,
    BX_ENV_SIGNAL_DEFAULT_NOERR,
    BX_ENV_SIGNAL_IGNORE,
    BX_ENV_SIGNAL_IGNORE_NOERR,
};

#define BX_ENV_SIGNAL_STORAGE 128

struct bx_env_signal_policy {
    enum bx_env_signal_mode modes[BX_ENV_SIGNAL_STORAGE];
    sigset_t block_signals;
    sigset_t unblock_signals;
    bool mask_changed;
    bool report_handling;
};

void bx_env_signal_policy_init(struct bx_env_signal_policy *policy);
bool bx_env_signal_parse_action(
    struct bx_env_signal_policy *policy,
    const char *argument,
    bool set_default,
    struct bx_diag_ctx *diag);
bool bx_env_signal_parse_block(
    struct bx_env_signal_policy *policy,
    const char *argument,
    bool block,
    struct bx_diag_ctx *diag);
bool bx_env_signal_apply(
    const struct bx_env_signal_policy *policy,
    bool debug,
    struct bx_diag_ctx *diag);
bool bx_env_signal_list(struct bx_diag_ctx *diag);

#endif
