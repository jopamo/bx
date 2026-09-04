#define _GNU_SOURCE
#include "lib/fetch/resume_validation.h"
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

MiraResumeAction mira_resume_action_for_status(int status_code) {
    if (status_code < 200)
        return MIRA_RESUME_ACTION_APPEND;
    if (status_code == 206)
        return MIRA_RESUME_ACTION_APPEND;
    if (status_code == 200)
        return MIRA_RESUME_ACTION_RESTART;
    return MIRA_RESUME_ACTION_DISCARD;
}

static int parse_nonnegative_long_long(const char** cursor, long long* value_out) {
    if (!cursor || !*cursor || !value_out)
        return -1;

    const char* p = *cursor;
    if (!isdigit((unsigned char)*p))
        return -1;

    errno = 0;
    char* end = NULL;
    long long value = strtoll(p, &end, 10);
    if (end == p || errno == ERANGE || value < 0)
        return -1;

    *cursor = end;
    *value_out = value;
    return 0;
}

int mira_parse_content_range(const char* content_range, MiraContentRange* range_out) {
    if (!content_range || !range_out)
        return -1;

    MiraContentRange parsed = {0};
    const char* p = content_range;
    while (*p && isspace((unsigned char)*p))
        p++;

    if (strncasecmp(p, "bytes", 5) != 0)
        return -1;
    p += 5;

    while (*p && isspace((unsigned char)*p))
        p++;

    if (parse_nonnegative_long_long(&p, &parsed.start) != 0)
        return -1;
    if (*p != '-')
        return -1;
    p++;

    if (parse_nonnegative_long_long(&p, &parsed.end) != 0)
        return -1;
    if (parsed.end < parsed.start)
        return -1;
    if (*p != '/')
        return -1;
    p++;

    if (*p == '*') {
        parsed.complete_length_known = false;
        p++;
    }
    else {
        if (parse_nonnegative_long_long(&p, &parsed.complete_length) != 0)
            return -1;
        if (parsed.complete_length <= parsed.end)
            return -1;
        parsed.complete_length_known = true;
    }

    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '\0')
        return -1;

    *range_out = parsed;
    return 0;
}

int mira_parse_content_range_start(const char* content_range, long long* start_out) {
    if (!content_range || !start_out)
        return -1;

    MiraContentRange parsed = {0};
    if (mira_parse_content_range(content_range, &parsed) != 0)
        return -1;

    *start_out = parsed.start;
    return 0;
}

bool mira_resume_content_range_matches(const char* content_range, long long expected_start) {
    MiraContentRange parsed = {0};
    return mira_parse_content_range(content_range, &parsed) == 0 && parsed.start == expected_start;
}

bool mira_resume_restart_preserves_verified_prefix(long long verified_prefix_bytes, long long replacement_body_bytes) {
    if (verified_prefix_bytes < 0 || replacement_body_bytes < 0)
        return false;
    return replacement_body_bytes >= verified_prefix_bytes;
}
