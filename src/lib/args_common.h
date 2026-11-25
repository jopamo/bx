#ifndef BX_COMMON_ARGS_COMMON_H
#define BX_COMMON_ARGS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <getopt.h>

#include "update_policy.h"

void bx_args_getopt_reset_at(int first_option_index);
void bx_args_getopt_reset(void);
enum { BX_ARGS_OPT_HELP = 512, BX_ARGS_OPT_VERSION };
int bx_args_getopt_long(
    int argc,
    char* const argv[],
    const char* short_options,
    const struct option* long_options,
    int* long_index
);

/* Opt in only when options apply to the entire command, not to a child
 * command or to individual operands. Use a leading '-' in short_options,
 * reset getopt, and initialize next_operand to 1. Reserve return value 1 for
 * positional operands. A leading '+' instead stops at the first operand.
 * Options return in input order. On -1,
 * argv[optind..argc) contains the operands in their original order, including
 * the literal tail after --. Earlier argv slots may change. */
int bx_args_getopt_long_interspersed(int argc, char* argv[], const char* short_options,
                                    const struct option* long_options, int* next_operand);

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
