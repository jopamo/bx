#ifndef BX_COMMON_ARGS_COMMON_H
#define BX_COMMON_ARGS_COMMON_H

#include <stdbool.h>
#include "update_policy.h"

bool bx_args_parse_preserve_list(const char* arg, unsigned* mask, bool set_bits, bool* mode_mentioned_out, char** invalid_token_out);

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
