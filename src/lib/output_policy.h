#ifndef BX_LIB_OUTPUT_POLICY_H
#define BX_LIB_OUTPUT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "lib/color.h"

enum bx_output_policy_format {
    BX_OUTPUT_POLICY_FORMAT_TEXT = 0,
    BX_OUTPUT_POLICY_FORMAT_JSON,
};

enum bx_output_policy_raw_exception {
    BX_OUTPUT_POLICY_RAW_MATCHED_CONTENT = UINT32_C(1) << 0,
    BX_OUTPUT_POLICY_RAW_MACHINE_DELIMITED_PATHS = UINT32_C(1) << 1,
    BX_OUTPUT_POLICY_RAW_EXPLICIT_PATH_FIELDS = UINT32_C(1) << 2,
    BX_OUTPUT_POLICY_RAW_USER_TEMPLATE = UINT32_C(1) << 3,
    BX_OUTPUT_POLICY_RAW_APPLET_TERMINAL_CONTROLS = UINT32_C(1) << 4,
    BX_OUTPUT_POLICY_RAW_FULL_RECORD = UINT32_C(1) << 5,
};

struct bx_output_policy {
    int fd;
    enum bx_output_policy_format format;
    enum bx_color_mode color_mode;
    bool nul_terminated;
    bool terminal_escape_paths;
    uint32_t raw_exceptions;
};

void bx_output_policy_init(struct bx_output_policy *policy, int fd);
void bx_output_policy_init_stdout(struct bx_output_policy *policy);
void bx_output_policy_set_json(struct bx_output_policy *policy, bool enabled);
bool bx_output_policy_json_requested(const struct bx_output_policy *policy);
void bx_output_policy_set_color_mode(struct bx_output_policy *policy, enum bx_color_mode mode);
void bx_output_policy_apply_color_mode(struct bx_output_policy *policy, enum bx_color_mode mode);
void bx_output_policy_apply_no_color(struct bx_output_policy *policy);
bool bx_output_policy_color_enabled(const struct bx_output_policy *policy);
void bx_output_policy_set_nul_terminated(struct bx_output_policy *policy, bool enabled);
char bx_output_policy_record_terminator(const struct bx_output_policy *policy);
void bx_output_policy_allow_raw_exception(struct bx_output_policy *policy,
                                          enum bx_output_policy_raw_exception exception);
bool bx_output_policy_has_raw_exception(const struct bx_output_policy *policy,
                                        enum bx_output_policy_raw_exception exception);
bool bx_output_policy_terminal_quote_paths(const struct bx_output_policy *policy);

#endif /* BX_LIB_OUTPUT_POLICY_H */
