#ifndef BX_COMMON_BACKUP_OPS_H
#define BX_COMMON_BACKUP_OPS_H

#include "args_common.h"
#include "diag.h"

struct bx_backup_params {
    enum bx_backup_mode mode;
    const char* suffix;
};

enum bx_backup_create_result {
    BX_BACKUP_CREATE_SKIPPED = 0,
    BX_BACKUP_CREATE_CREATED,
    BX_BACKUP_CREATE_FAILED,
};

/*
 * bx_backup_get_params: determine backup mode and suffix from args and environment.
 */
void bx_backup_get_params(enum bx_backup_mode cmd_mode, const char* cmd_suffix, struct bx_backup_params* out);

/*
 * bx_backup_create: attempt to rename 'path' to a backup name.
 * Returns the operation status and stores the backup name in backup_path_out on success.
 */
enum bx_backup_create_result bx_backup_create(const char* path, const struct bx_backup_params* params, struct bx_diag_ctx* diag, char** backup_path_out);

/*
 * bx_backup_create_copy: attempt to copy 'path' to a backup name.
 * Returns the operation status and stores the backup name in backup_path_out on success.
 */
enum bx_backup_create_result bx_backup_create_copy(const char* path, const struct bx_backup_params* params, struct bx_diag_ctx* diag, char** backup_path_out);

#endif /* BX_COMMON_BACKUP_OPS_H */
