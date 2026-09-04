#define _GNU_SOURCE
#include "lib/fetch/timestamp_policy.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

static bool parse_http_date_format(const char* value, const char* format, struct tm* tm_out) {
    if (!value || !format || !tm_out)
        return false;

    struct tm parsed;
    memset(&parsed, 0, sizeof(parsed));

    char* end = strptime(value, format, &parsed);
    if (!end || *end != '\0')
        return false;

    parsed.tm_isdst = 0;
    *tm_out = parsed;
    return true;
}

static void normalize_rfc850_year(struct tm* tm_value) {
    if (!tm_value)
        return;
    if (tm_value->tm_year < 0 || tm_value->tm_year >= 100)
        return;

    time_t now = time(NULL);
    struct tm now_utc;
    if (now == (time_t)-1 || gmtime_r(&now, &now_utc) == NULL) {
        tm_value->tm_year += 100;
        return;
    }

    int candidate_year = 1900 + tm_value->tm_year;
    int current_year = 1900 + now_utc.tm_year;
    while (candidate_year < current_year - 50) {
        candidate_year += 100;
    }
    while (candidate_year > current_year + 50) {
        candidate_year -= 100;
    }

    tm_value->tm_year = candidate_year - 1900;
}

static bool parse_http_date(const char* value, time_t* out) {
    if (!value || !out)
        return false;

    while (isspace((unsigned char)*value))
        value++;
    if (*value == '\0')
        return false;

    struct tm tm_value;
    if (parse_http_date_format(value, "%a, %d %b %Y %H:%M:%S GMT", &tm_value)) {
        time_t parsed = timegm(&tm_value);
        if (parsed == (time_t)-1)
            return false;
        *out = parsed;
        return true;
    }

    if (parse_http_date_format(value, "%A, %d-%b-%y %H:%M:%S GMT", &tm_value)) {
        normalize_rfc850_year(&tm_value);
        time_t parsed = timegm(&tm_value);
        if (parsed == (time_t)-1)
            return false;
        *out = parsed;
        return true;
    }

    if (parse_http_date_format(value, "%a %b %e %H:%M:%S %Y", &tm_value)) {
        time_t parsed = timegm(&tm_value);
        if (parsed == (time_t)-1)
            return false;
        *out = parsed;
        return true;
    }

    return false;
}

bool mira_timestamp_should_use_server_time(bool no_use_server_timestamps, int status, const char* output_path, const char* last_modified_header, time_t* server_mtime_out) {
    if (!output_path)
        return false;
    if (no_use_server_timestamps)
        return false;
    if (!(status == 200 || status == 206))
        return false;
    if (strcmp(output_path, "-") == 0)
        return false;
    if (!last_modified_header || last_modified_header[0] == '\0')
        return false;

    time_t parsed = 0;
    if (!parse_http_date(last_modified_header, &parsed))
        return false;

    if (server_mtime_out) {
        *server_mtime_out = parsed;
    }
    return true;
}
