#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lib/checked_math.h"
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
        .clamp_positive_underflow = false,
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
    int parse_errno = errno;
    if (options->require_strtod_range && parse_errno != 0) {
        return false;
    }
    const char* sign = text;
    while (isspace((unsigned char)*sign)) {
        sign++;
    }
    if (parse_errno == ERANGE && value == 0.0 && *sign == '-') {
        return false;
    }
    if (options->clamp_positive_underflow && parse_errno == ERANGE &&
        value == 0.0) {
        value = nextafter(0.0, 1.0);
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

bool bx_time_milliseconds_to_timespec(intmax_t milliseconds, struct timespec* ts_out) {
    if (ts_out == NULL || milliseconds < 0) {
        return false;
    }

    intmax_t seconds_value = milliseconds / 1000;
    intmax_t milliseconds_remainder = milliseconds % 1000;
    time_t tv_sec = 0;
    if (!bx_checked_intmax_to_time_t(seconds_value, &tv_sec)) {
        return false;
    }

    ts_out->tv_sec = tv_sec;
    ts_out->tv_nsec = (long)milliseconds_remainder * 1000000L;
    return true;
}

bool bx_time_milliseconds_to_seconds_double(intmax_t milliseconds, double* seconds_out) {
    if (seconds_out == NULL || milliseconds < 0) {
        return false;
    }

    double seconds = (double)milliseconds / 1000.0;
    if (!isfinite(seconds)) {
        return false;
    }

    *seconds_out = seconds;
    return true;
}

bool bx_time_milliseconds_double_to_seconds_double(double milliseconds, double* seconds_out) {
    if (seconds_out == NULL || !isfinite(milliseconds) || milliseconds < 0.0) {
        return false;
    }

    double seconds = milliseconds / 1000.0;
    if (!isfinite(seconds)) {
        return false;
    }

    *seconds_out = seconds;
    return true;
}

bool bx_time_milliseconds_to_seconds_int_floor(intmax_t milliseconds, int* seconds_out) {
    if (seconds_out == NULL || milliseconds < 0) {
        return false;
    }

    intmax_t seconds = milliseconds / 1000;
    if (seconds > INT_MAX) {
        return false;
    }

    *seconds_out = (int)seconds;
    return true;
}

bool bx_time_seconds_to_milliseconds_uint(uintmax_t seconds, uintmax_t* milliseconds_out) {
    return bx_checked_uintmax_mul(seconds, 1000u, milliseconds_out);
}

bool bx_time_seconds_to_milliseconds_int(uintmax_t seconds, int* milliseconds_out) {
    uintmax_t milliseconds = 0;

    if (milliseconds_out == NULL ||
        !bx_time_seconds_to_milliseconds_uint(seconds, &milliseconds) ||
        milliseconds > (uintmax_t)INT_MAX) {
        return false;
    }

    *milliseconds_out = (int)milliseconds;
    return true;
}

bool bx_time_seconds_to_milliseconds_int_ceil(double seconds, int* milliseconds_out) {
    if (milliseconds_out == NULL || !isfinite(seconds) || seconds < 0.0) {
        return false;
    }

    double milliseconds = ceil(seconds * 1000.0);
    if (!isfinite(milliseconds) || milliseconds < 0.0 || milliseconds > (double)INT_MAX) {
        return false;
    }

    *milliseconds_out = (int)milliseconds;
    return true;
}

bool bx_time_seconds_to_milliseconds_double(double seconds, double* milliseconds_out) {
    if (milliseconds_out == NULL || !isfinite(seconds)) {
        return false;
    }

    double milliseconds = seconds * 1000.0;
    if (!isfinite(milliseconds)) {
        return false;
    }

    *milliseconds_out = milliseconds;
    return true;
}

bool bx_time_timespec_to_nanoseconds_u64(const struct timespec* ts, uint64_t* nanoseconds_out) {
    if (ts == NULL || nanoseconds_out == NULL ||
        (((time_t)-1 < (time_t)0) && ts->tv_sec < (time_t)0) ||
        ts->tv_nsec < 0L || ts->tv_nsec >= 1000000000L) {
        return false;
    }

    uintmax_t seconds = (uintmax_t)ts->tv_sec;
    uintmax_t nanoseconds = (uintmax_t)ts->tv_nsec;
    uintmax_t seconds_nanoseconds = 0;
    uintmax_t total_nanoseconds = 0;
    if (!bx_checked_uintmax_mul(seconds, UINT64_C(1000000000), &seconds_nanoseconds) ||
        !bx_checked_uintmax_add(seconds_nanoseconds, nanoseconds, &total_nanoseconds) ||
        total_nanoseconds > (uintmax_t)UINT64_MAX) {
        return false;
    }

    *nanoseconds_out = (uint64_t)total_nanoseconds;
    return true;
}

bool bx_time_timespec_to_seconds_double(const struct timespec* ts, double* seconds_out) {
    if (ts == NULL || seconds_out == NULL || ts->tv_nsec < 0L || ts->tv_nsec >= 1000000000L) {
        return false;
    }

    double seconds = (double)ts->tv_sec + ((double)ts->tv_nsec / 1000000000.0);
    if (!isfinite(seconds)) {
        return false;
    }

    *seconds_out = seconds;
    return true;
}

bool bx_time_timeval_to_seconds_double(const struct timeval* tv, double* seconds_out) {
    if (tv == NULL || seconds_out == NULL || tv->tv_usec < 0 || tv->tv_usec >= 1000000) {
        return false;
    }

    double seconds = (double)tv->tv_sec + ((double)tv->tv_usec / 1000000.0);
    if (!isfinite(seconds)) {
        return false;
    }

    *seconds_out = seconds;
    return true;
}

bool bx_time_timeval_elapsed_seconds_double(const struct timeval* start, const struct timeval* end, double* seconds_out) {
    double start_seconds = 0.0;
    double end_seconds = 0.0;

    if (seconds_out == NULL ||
        !bx_time_timeval_to_seconds_double(start, &start_seconds) ||
        !bx_time_timeval_to_seconds_double(end, &end_seconds) ||
        end_seconds < start_seconds) {
        return false;
    }

    double elapsed = end_seconds - start_seconds;
    if (!isfinite(elapsed)) {
        return false;
    }

    *seconds_out = elapsed;
    return true;
}

bool bx_time_timeval_to_milliseconds_int64(const struct timeval* tv, int64_t* milliseconds_out) {
    if (tv == NULL || milliseconds_out == NULL || tv->tv_usec < 0 || tv->tv_usec >= 1000000) {
        return false;
    }

    long double milliseconds = ((long double)tv->tv_sec * 1000.0L) + ((long double)tv->tv_usec / 1000.0L);
    if (milliseconds < (long double)INT64_MIN || milliseconds > (long double)INT64_MAX) {
        return false;
    }

    *milliseconds_out = (int64_t)milliseconds;
    return true;
}

bool bx_time_timeval_elapsed_milliseconds_int64(const struct timeval* start, const struct timeval* end, int64_t* milliseconds_out) {
    int64_t start_ms = 0;
    int64_t end_ms = 0;

    if (milliseconds_out == NULL ||
        !bx_time_timeval_to_milliseconds_int64(start, &start_ms) ||
        !bx_time_timeval_to_milliseconds_int64(end, &end_ms) ||
        end_ms < start_ms) {
        return false;
    }

    *milliseconds_out = end_ms - start_ms;
    return true;
}

bool bx_time_timespec_to_milliseconds_double(const struct timespec* ts, double* milliseconds_out) {
    if (ts == NULL || milliseconds_out == NULL || ts->tv_nsec < 0L || ts->tv_nsec >= 1000000000L) {
        return false;
    }

    double milliseconds = ((double)ts->tv_sec * 1000.0) + ((double)ts->tv_nsec / 1000000.0);
    if (!isfinite(milliseconds)) {
        return false;
    }

    *milliseconds_out = milliseconds;
    return true;
}

bool bx_time_timespec_to_milliseconds_uint(const struct timespec* ts, uintmax_t* milliseconds_out) {
    if (ts == NULL || milliseconds_out == NULL || ts->tv_nsec < 0L || ts->tv_nsec >= 1000000000L) {
        return false;
    }

    long double seconds_as_long_double = (long double)ts->tv_sec;
    if (seconds_as_long_double < 0.0L || seconds_as_long_double > (long double)UINTMAX_MAX) {
        return false;
    }

    uintmax_t seconds = (uintmax_t)ts->tv_sec;
    uintmax_t nanoseconds_milliseconds = (uintmax_t)(ts->tv_nsec / 1000000L);
    uintmax_t seconds_milliseconds = 0;
    if (!bx_checked_uintmax_mul(seconds, 1000u, &seconds_milliseconds) ||
        !bx_checked_uintmax_add(seconds_milliseconds, nanoseconds_milliseconds, milliseconds_out)) {
        return false;
    }

    return true;
}

bool bx_time_timespec_elapsed_milliseconds_int64(const struct timespec* start, const struct timespec* end, int64_t* milliseconds_out) {
    uintmax_t start_ms = 0;
    uintmax_t end_ms = 0;

    if (milliseconds_out == NULL ||
        !bx_time_timespec_to_milliseconds_uint(start, &start_ms) ||
        !bx_time_timespec_to_milliseconds_uint(end, &end_ms) ||
        end_ms < start_ms) {
        return false;
    }

    uintmax_t elapsed_ms = end_ms - start_ms;
    if (elapsed_ms > (uintmax_t)INT64_MAX) {
        return false;
    }

    *milliseconds_out = (int64_t)elapsed_ms;
    return true;
}

bool bx_time_timespec_elapsed_seconds_double(const struct timespec* start, const struct timespec* end, double* seconds_out) {
    if (start == NULL || end == NULL || seconds_out == NULL ||
        start->tv_nsec < 0L || start->tv_nsec >= 1000000000L ||
        end->tv_nsec < 0L || end->tv_nsec >= 1000000000L) {
        return false;
    }

    long double elapsed = ((long double)end->tv_sec - (long double)start->tv_sec) +
                          (((long double)end->tv_nsec - (long double)start->tv_nsec) / 1000000000.0L);
    if (elapsed < 0.0L) {
        return false;
    }

    double seconds = (double)elapsed;
    if (!isfinite(seconds)) {
        return false;
    }

    *seconds_out = seconds;
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

    if (text == NULL || text[0] != '@' || text[1] == '\0' || ts_out == NULL) {
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

    time_t seconds = 0;
    if (!bx_checked_intmax_to_time_t(seconds_value, &seconds)) {
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
