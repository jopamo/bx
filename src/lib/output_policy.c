#include <unistd.h>

#include "lib/output_policy.h"
#include "lib/output_quote.h"

void bx_output_policy_init(struct bx_output_policy *policy, int fd) {
    if (!policy) {
        return;
    }
    policy->fd = fd;
    policy->format = BX_OUTPUT_POLICY_FORMAT_TEXT;
    policy->color_mode = BX_COLOR_AUTO;
    policy->nul_terminated = false;
    policy->terminal_escape_paths = bx_output_quote_terminal_should_hide_control(fd);
    policy->raw_exceptions = 0u;
}

void bx_output_policy_init_stdout(struct bx_output_policy *policy) {
    bx_output_policy_init(policy, STDOUT_FILENO);
}

void bx_output_policy_set_json(struct bx_output_policy *policy, bool enabled) {
    if (!policy) {
        return;
    }
    policy->format = enabled ? BX_OUTPUT_POLICY_FORMAT_JSON : BX_OUTPUT_POLICY_FORMAT_TEXT;
}

bool bx_output_policy_json_requested(const struct bx_output_policy *policy) {
    return policy && policy->format == BX_OUTPUT_POLICY_FORMAT_JSON;
}

void bx_output_policy_set_color_mode(struct bx_output_policy *policy, enum bx_color_mode mode) {
    if (!policy) {
        return;
    }
    policy->color_mode = mode;
}

void bx_output_policy_apply_color_mode(struct bx_output_policy *policy, enum bx_color_mode mode) {
    if (!policy) {
        return;
    }
    bx_output_policy_set_color_mode(policy, mode);
    bx_color_set_mode(mode);
}

void bx_output_policy_apply_no_color(struct bx_output_policy *policy) {
    bx_output_policy_apply_color_mode(policy, BX_COLOR_NEVER);
}

bool bx_output_policy_color_enabled(const struct bx_output_policy *policy) {
    if (!policy) {
        return false;
    }
    switch (policy->color_mode) {
        case BX_COLOR_ALWAYS:
            return true;
        case BX_COLOR_AUTO:
            return isatty(policy->fd) == 1;
        case BX_COLOR_NEVER:
        default:
            return false;
    }
}

void bx_output_policy_set_nul_terminated(struct bx_output_policy *policy, bool enabled) {
    if (!policy) {
        return;
    }
    policy->nul_terminated = enabled;
}

char bx_output_policy_record_terminator(const struct bx_output_policy *policy) {
    return policy && policy->nul_terminated ? '\0' : '\n';
}

void bx_output_policy_allow_raw_exception(struct bx_output_policy *policy,
                                          enum bx_output_policy_raw_exception exception) {
    if (!policy) {
        return;
    }
    policy->raw_exceptions |= (uint32_t)exception;
}

bool bx_output_policy_has_raw_exception(const struct bx_output_policy *policy,
                                        enum bx_output_policy_raw_exception exception) {
    return policy && (policy->raw_exceptions & (uint32_t)exception) != 0u;
}

bool bx_output_policy_terminal_quote_paths(const struct bx_output_policy *policy) {
    if (!policy || !policy->terminal_escape_paths) {
        return false;
    }
    if (bx_output_policy_json_requested(policy) || policy->nul_terminated) {
        return false;
    }
    if (bx_output_policy_has_raw_exception(policy, BX_OUTPUT_POLICY_RAW_MACHINE_DELIMITED_PATHS)
        || bx_output_policy_has_raw_exception(policy, BX_OUTPUT_POLICY_RAW_EXPLICIT_PATH_FIELDS)
        || bx_output_policy_has_raw_exception(policy, BX_OUTPUT_POLICY_RAW_USER_TEMPLATE)
        || bx_output_policy_has_raw_exception(policy, BX_OUTPUT_POLICY_RAW_FULL_RECORD)) {
        return false;
    }
    return true;
}
