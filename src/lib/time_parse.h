#ifndef BX_COMMON_TIME_PARSE_H
#define BX_COMMON_TIME_PARSE_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/time.h>
#include <time.h>

struct bx_time_epoch_parse_options {
    bool allow_trailing_space;
    bool normalize_negative_fraction;
};

struct bx_time_duration_parse_options {
    bool allow_infinite;
    bool require_strtod_range;
};

struct bx_time_duration_parse_result {
    double seconds;
    bool infinite;
};

bool bx_time_parse_fixed_width_int(const char* text, size_t start, size_t width, int* value_out);
bool bx_time_parse_fractional_nanoseconds(const char** text, long* nsec_out);
bool bx_time_duration_suffix_multiplier(char suffix, double* multiplier_out);
bool bx_time_parse_duration_seconds(const char* text, const struct bx_time_duration_parse_options* options, struct bx_time_duration_parse_result* result_out);
bool bx_time_seconds_to_timespec(double seconds, struct timespec* ts_out);
bool bx_time_milliseconds_to_timespec(intmax_t milliseconds, struct timespec* ts_out);
bool bx_time_seconds_to_milliseconds_uint(uintmax_t seconds, uintmax_t* milliseconds_out);
bool bx_time_seconds_to_milliseconds_int(uintmax_t seconds, int* milliseconds_out);
bool bx_time_seconds_to_milliseconds_int_ceil(double seconds, int* milliseconds_out);
bool bx_time_seconds_to_milliseconds_double(double seconds, double* milliseconds_out);
bool bx_time_timespec_to_seconds_double(const struct timespec* ts, double* seconds_out);
bool bx_time_timeval_to_seconds_double(const struct timeval* tv, double* seconds_out);
bool bx_time_timeval_elapsed_seconds_double(const struct timeval* start, const struct timeval* end, double* seconds_out);
bool bx_time_timeval_to_milliseconds_int64(const struct timeval* tv, int64_t* milliseconds_out);
bool bx_time_timeval_elapsed_milliseconds_int64(const struct timeval* start, const struct timeval* end, int64_t* milliseconds_out);
bool bx_time_timespec_to_milliseconds_double(const struct timespec* ts, double* milliseconds_out);
bool bx_time_timespec_to_milliseconds_uint(const struct timespec* ts, uintmax_t* milliseconds_out);
bool bx_time_timespec_elapsed_milliseconds_int64(const struct timespec* start, const struct timespec* end, int64_t* milliseconds_out);
bool bx_time_timespec_elapsed_seconds_double(const struct timespec* start, const struct timespec* end, double* seconds_out);
bool bx_time_parse_epoch_literal(const char* text, const struct bx_time_epoch_parse_options* options, struct timespec* ts_out);
bool bx_time_build_local_timestamp(int year, int month, int day, int hour, int minute, int second, long nsec, struct timespec* timestamp_out);
bool bx_time_current_local_year(int* year_out);

#endif /* BX_COMMON_TIME_PARSE_H */
