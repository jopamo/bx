#ifndef BX_COMMON_TIME_PARSE_H
#define BX_COMMON_TIME_PARSE_H

#include <stdbool.h>
#include <stddef.h>
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
bool bx_time_parse_epoch_literal(const char* text, const struct bx_time_epoch_parse_options* options, struct timespec* ts_out);
bool bx_time_build_local_timestamp(int year, int month, int day, int hour, int minute, int second, long nsec, struct timespec* timestamp_out);
bool bx_time_current_local_year(int* year_out);

#endif /* BX_COMMON_TIME_PARSE_H */
