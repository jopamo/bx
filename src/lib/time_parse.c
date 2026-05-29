#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
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

bool bx_time_duration_suffix_multiplier(char suffix, double* multiplier_out) {
    if (multiplier_out == NULL) {
        return false;
    }

    switch (suffix) {
        case '\0':
        case 's':
            *multiplier_out = 1.0;
            return true;
        case 'm':
            *multiplier_out = 60.0;
            return true;
        case 'h':
            *multiplier_out = 3600.0;
            return true;
        case 'd':
            *multiplier_out = 86400.0;
            return true;
        default:
            return false;
    }
}

bool bx_time_parse_duration_seconds(const char* text, const struct bx_time_duration_parse_options* options, struct bx_time_duration_parse_result* result_out) {
    const struct bx_time_duration_parse_options default_options = {
        .allow_infinite = false,
        .require_strtod_range = true,
    };
    if (options == NULL) {
        options = &default_options;
    }

    if (text == NULL || text[0] == '\0' || result_out == NULL) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    double value = strtod(text, &end);
    if (end == text || isnan(value)) {
        return false;
    }
    if (options->require_strtod_range && errno != 0) {
        return false;
    }

    double multiplier = 1.0;
    if (end[0] != '\0' && end[1] != '\0') {
        return false;
    }
    if (!bx_time_duration_suffix_multiplier(*end, &multiplier)) {
        return false;
    }

    double seconds = value * multiplier;
    if (isnan(seconds) || seconds < 0.0) {
        return false;
    }
    if (isinf(seconds)) {
        if (!options->allow_infinite) {
            return false;
        }
        result_out->seconds = 0.0;
        result_out->infinite = true;
        return true;
    }
    if (!isfinite(seconds)) {
        return false;
    }

    result_out->seconds = seconds;
    result_out->infinite = false;
    return true;
}

static long double bx_time_time_t_max_value(void) {
    const int value_bits = (int)(sizeof(time_t) * CHAR_BIT) - (((time_t)-1 < (time_t)0) ? 1 : 0);
    return ldexpl(1.0L, value_bits) - 1.0L;
}

bool bx_time_seconds_to_timespec(double seconds, struct timespec* ts_out) {
    if (ts_out == NULL || !isfinite(seconds) || seconds < 0.0) {
        return false;
    }

    double whole_seconds = 0.0;
    double fractional_seconds = modf(seconds, &whole_seconds);
    if (whole_seconds < 0.0 || (long double)whole_seconds > bx_time_time_t_max_value()) {
        return false;
    }

    time_t tv_sec = (time_t)whole_seconds;
    if ((long double)tv_sec != (long double)whole_seconds) {
        return false;
    }

    long tv_nsec = (long)(fractional_seconds * 1000000000.0);
    if (tv_nsec < 0L || tv_nsec >= 1000000000L) {
        return false;
    }

    ts_out->tv_sec = tv_sec;
    ts_out->tv_nsec = tv_nsec;
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
    intmax_t seconds_value = strtoimax(text + 1, &end, 10);
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

    if (seconds_value < 0 && nsec != 0) {
        if (!options->normalize_negative_fraction) {
            return false;
        }
        if (seconds_value == INTMAX_MIN) {
            return false;
        }
        seconds_value -= 1;
        nsec = 1000000000L - nsec;
    }

    time_t seconds = (time_t)seconds_value;
    if ((intmax_t)seconds != seconds_value) {
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
