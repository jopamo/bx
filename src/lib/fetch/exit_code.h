#ifndef MIRA_EXIT_CODE_H
#define MIRA_EXIT_CODE_H

/* MIRA_HEADER_OWNER: util */
/* MIRA_HEADER_CONSUMERS: util, entry, core */

/*
 * Layering contract:
 * - Exit-code translation is centralized here so entry/core code does not keep
 *   divergent mappings.
 *
 * Ownership and lifetime:
 * - mira_exit_code_table()/mira_exit_code_info() return pointers to immutable
 *   static data; callers must not free or mutate them.
 * - Mapping helpers are pure value translations.
 */

#include "error.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MIRA_EXIT_SUCCESS = 0,
    MIRA_EXIT_PARSE_OR_CONFIG = 2,
    MIRA_EXIT_FILE_IO = 3,
    MIRA_EXIT_NETWORK = 4,
    MIRA_EXIT_SSL = 5,
    MIRA_EXIT_AUTH = 6,
    MIRA_EXIT_PROTOCOL = 7,
    MIRA_EXIT_SERVER = 8,
    MIRA_EXIT_POLICY = 9,
} MiraExitCode;

typedef struct {
    MiraExitCode code;
    const char* label;
    const char* description;
} MiraExitCodeInfo;

const MiraExitCodeInfo* mira_exit_code_table(size_t* count);
const MiraExitCodeInfo* mira_exit_code_info(int code);
bool mira_exit_code_is_assigned(int code);
int mira_exit_code_for_error_class(MiraErrorClass class_id, int http_status);
int mira_exit_code_for_transfer_failure(int http_status, MiraTransportErrorKind transport_kind, MiraError result);
MiraErrorClass mira_error_class_for_exit_code(int exit_code);

static inline int mira_exit_combine(int best, int code) {
    if (code <= 0)
        return best;
    if (best == 0)
        return code;
    return (code < best) ? code : best;
}

#endif  // MIRA_EXIT_CODE_H
