#ifndef BX_FETCH_EXIT_CODE_H
#define BX_FETCH_EXIT_CODE_H

/* BX_FETCH_HEADER_OWNER: util */
/* BX_FETCH_HEADER_CONSUMERS: util, entry, core */

/*
 * Layering contract:
 * - Exit-code translation is centralized here so entry/core code does not keep
 *   divergent mappings.
 *
 * Ownership and lifetime:
 * - bx_fetch_exit_code_table()/bx_fetch_exit_code_info() return pointers to immutable
 *   static data; callers must not free or mutate them.
 * - Mapping helpers are pure value translations.
 */

#include "error.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    BX_FETCH_EXIT_SUCCESS = 0,
    BX_FETCH_EXIT_PARSE_OR_CONFIG = 2,
    BX_FETCH_EXIT_FILE_IO = 3,
    BX_FETCH_EXIT_NETWORK = 4,
    BX_FETCH_EXIT_SSL = 5,
    BX_FETCH_EXIT_AUTH = 6,
    BX_FETCH_EXIT_PROTOCOL = 7,
    BX_FETCH_EXIT_SERVER = 8,
    BX_FETCH_EXIT_POLICY = 9,
} BxFetchExitCode;

typedef struct {
    BxFetchExitCode code;
    const char* label;
    const char* description;
} BxFetchExitCodeInfo;

const BxFetchExitCodeInfo* bx_fetch_exit_code_table(size_t* count);
const BxFetchExitCodeInfo* bx_fetch_exit_code_info(int code);
bool bx_fetch_exit_code_is_assigned(int code);
int bx_fetch_exit_code_for_error_class(BxFetchErrorClass class_id, int http_status);
int bx_fetch_exit_code_for_transfer_failure(int http_status, BxFetchTransportErrorKind transport_kind, BxFetchError result);
BxFetchErrorClass bx_fetch_error_class_for_exit_code(int exit_code);

static inline int bx_fetch_exit_combine(int best, int code) {
    if (code <= 0)
        return best;
    if (best == 0)
        return code;
    return (code < best) ? code : best;
}

#endif  // BX_FETCH_EXIT_CODE_H
