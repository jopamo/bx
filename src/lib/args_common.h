#ifndef BX_COMMON_ARGS_COMMON_H
#define BX_COMMON_ARGS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <getopt.h>

#include "update_policy.h"

void bx_args_getopt_reset_at(int first_option_index);
void bx_args_getopt_reset(void);
int bx_args_getopt_long(
    int argc,
    char* const argv[],
    const char* short_options,
    const struct option* long_options,
    int* long_index
);

bool bx_args_parse_preserve_list(const char* arg, unsigned* mask, bool set_bits, bool* mode_mentioned_out, char** invalid_token_out);

bool bx_args_parse_int_range(const char* arg, int min_value, int max_value, int* value_out);
bool bx_args_parse_llong_range(const char* arg, long long min_value, long long max_value, long long* value_out);
bool bx_args_parse_size_range(const char* arg, size_t min_value, size_t max_value, size_t* value_out);

bool bx_args_parse_update_mode(const char* arg, enum bx_update_mode* mode_out);

enum bx_backup_mode {
    BX_BACKUP_NONE = 0,
    BX_BACKUP_UNSPECIFIED,
    BX_BACKUP_OFF,
    BX_BACKUP_SIMPLE,
    BX_BACKUP_NUMBERED,
    BX_BACKUP_EXISTING,
};

bool bx_args_parse_backup_mode(const char* arg, enum bx_backup_mode* mode_out);
void bx_args_enable_backup_mode(enum bx_backup_mode* mode);
bool bx_args_backup_mode_requested(enum bx_backup_mode mode);
bool bx_args_backup_mode_enabled(enum bx_backup_mode mode);

#endif /* BX_COMMON_ARGS_COMMON_H */
