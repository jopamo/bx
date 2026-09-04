#ifndef MIRA_RESUME_VALIDATION_H
#define MIRA_RESUME_VALIDATION_H

/* MIRA_HEADER_OWNER: net */
/* MIRA_HEADER_CONSUMERS: net */

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
    MIRA_RESUME_ACTION_APPEND = 0,
    MIRA_RESUME_ACTION_RESTART,
    MIRA_RESUME_ACTION_DISCARD,
} MiraResumeAction;

typedef struct {
    long long start;
    long long end;
    bool complete_length_known;
    long long complete_length;
} MiraContentRange;

MiraResumeAction mira_resume_action_for_status(int status_code);
int mira_parse_content_range(const char *content_range, MiraContentRange *range_out);
int mira_parse_content_range_start(const char *content_range, long long *start_out);
bool mira_resume_content_range_matches(const char *content_range, long long expected_start);
bool mira_resume_restart_preserves_verified_prefix(long long verified_prefix_bytes,
                                                   long long replacement_body_bytes);

#endif // MIRA_RESUME_VALIDATION_H
