#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lib/time_parse.h"

bool bx_time_parse_fixed_width_int(const char* text, size_t start, size_t width, int* value_out) {
    int value = 0;
    for (size_t i = 0; i < width; i++) {
        unsigned char ch = (unsigned char)text[start + i];
        if (!isdigit(ch)) {
            return false;
        }
        value = (value * 10) + (int)(ch - '0');
    }
    *value_out = value;
    return true;
}

bool bx_time_parse_fractional_nanoseconds(const char** text, long* nsec_out) {
    const char* p = *text;
    long nsec = 0;
    size_t digits = 0;

    if (*p != '.') {
        *nsec_out = 0;
        return true;
    }

    p++;
    if (!isdigit((unsigned char)*p)) {
        return false;
    }

    while (isdigit((unsigned char)*p)) {
        if (digits < 9u) {
            nsec = (nsec * 10L) + (long)(*p - '0');
        }
        digits++;
        p++;
    }

    while (digits < 9u) {
        nsec *= 10L;
        digits++;
    }

    *text = p;
    *nsec_out = nsec;
    return true;
}

bool bx_time_parse_epoch_literal(const char* text, const struct bx_time_epoch_parse_options* options, struct timespec* ts_out) {
    struct bx_time_epoch_parse_options default_options = {
        .allow_trailing_space = false,
        .normalize_negative_fraction = false,
    };
    if (options == NULL) {
        options = &default_options;
    }

    if (text == NULL || text[0] != '@' || text[1] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    long long seconds_ll = strtoll(text + 1, &end, 10);
    if (errno != 0 || end == text + 1) {
        return false;
    }

    const char* tail = end;
    long nsec = 0;
    if (*tail == '.') {
        if (!bx_time_parse_fractional_nanoseconds(&tail, &nsec)) {
            return false;
        }
    }

    if (options->allow_trailing_space) {
        while (isspace((unsigned char)*tail)) {
            tail++;
        }
    }
    if (*tail != '\0') {
        return false;
    }

    if (seconds_ll < 0 && nsec != 0) {
        if (!options->normalize_negative_fraction) {
            return false;
        }
        if (seconds_ll == LLONG_MIN) {
            return false;
        }
        seconds_ll -= 1;
        nsec = 1000000000L - nsec;
    }

    time_t seconds = (time_t)seconds_ll;
    if ((long long)seconds != seconds_ll) {
        return false;
    }

    ts_out->tv_sec = seconds;
    ts_out->tv_nsec = nsec;
    return true;
}

bool bx_time_build_local_timestamp(int year, int month, int day, int hour, int minute, int second, long nsec, struct timespec* timestamp_out) {
    if (year < 0 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60 || nsec < 0 ||
        nsec > 999999999L) {
        return false;
    }

    struct tm tm_value;
    memset(&tm_value, 0, sizeof(tm_value));
    tm_value.tm_year = year - 1900;
    tm_value.tm_mon = month - 1;
    tm_value.tm_mday = day;
    tm_value.tm_hour = hour;
    tm_value.tm_min = minute;
    tm_value.tm_sec = second;
    tm_value.tm_isdst = -1;

    int expected_year = tm_value.tm_year;
    int expected_mon = tm_value.tm_mon;
    int expected_day = tm_value.tm_mday;
    int expected_hour = tm_value.tm_hour;
    int expected_minute = tm_value.tm_min;
    int expected_second = tm_value.tm_sec;

    time_t seconds = mktime(&tm_value);
    (void)seconds;
    if (tm_value.tm_year != expected_year || tm_value.tm_mon != expected_mon || tm_value.tm_mday != expected_day || tm_value.tm_hour != expected_hour ||
        tm_value.tm_min != expected_minute || tm_value.tm_sec != expected_second) {
        return false;
    }

    timestamp_out->tv_sec = seconds;
    timestamp_out->tv_nsec = nsec;
    return true;
}

bool bx_time_current_local_year(int* year_out) {
    time_t now = time(NULL);
    struct tm local_tm;

    if (localtime_r(&now, &local_tm) == NULL) {
        return false;
    }

    *year_out = local_tm.tm_year + 1900;
    return true;
}
