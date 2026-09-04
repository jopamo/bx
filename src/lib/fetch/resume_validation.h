#ifndef BX_FETCH_RESUME_VALIDATION_H
#define BX_FETCH_RESUME_VALIDATION_H

/* BX_FETCH_HEADER_OWNER: net */
/* BX_FETCH_HEADER_CONSUMERS: net */

/*
 * Layering contract:
 * - Resume validation/parsing policy for HTTP range handling is centralized in
 *   net to keep transfer state-machine checks consistent.
 *
 * Ownership and lifetime:
 * - APIs are value-based; no dynamic ownership transfer.
 * - Output structs/values are caller-owned.
 */

#include <stdbool.h>

typedef enum {
    BX_FETCH_RESUME_ACTION_APPEND = 0,
    BX_FETCH_RESUME_ACTION_RESTART,
    BX_FETCH_RESUME_ACTION_DISCARD,
} MiraResumeAction;

typedef struct {
    long long start;
    long long end;
    bool complete_length_known;
    long long complete_length;
} MiraContentRange;

MiraResumeAction bx_fetch_resume_action_for_status(int status_code);
int bx_fetch_parse_content_range(const char* content_range, MiraContentRange* range_out);
int bx_fetch_parse_content_range_start(const char* content_range, long long* start_out);
bool bx_fetch_resume_content_range_matches(const char* content_range, long long expected_start);
bool bx_fetch_resume_restart_preserves_verified_prefix(long long verified_prefix_bytes, long long replacement_body_bytes);

#endif  // BX_FETCH_RESUME_VALIDATION_H
