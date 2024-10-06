#ifndef BX_COMMON_BACKUP_OPS_H
#define BX_COMMON_BACKUP_OPS_H

#include "args_common.h"
#include "diag.h"

struct bx_backup_params {
    enum bx_backup_mode mode;
    const char *suffix;
};

/*
 * bx_backup_get_params: determine backup mode and suffix from args and environment.
 */
void bx_backup_get_params(enum bx_backup_mode cmd_mode,
                          const char *cmd_suffix,
                          struct bx_backup_params *out);

/*
 * bx_backup_create: attempt to rename 'path' to a backup name.
 * Returns the backup name (to be freed) on success, NULL on failure or if no backup needed.
 */
char *bx_backup_create(const char *path, const struct bx_backup_params *params, struct bx_diag_ctx *diag);

/*
 * bx_backup_create_copy: attempt to copy 'path' to a backup name.
 * Returns the backup name (to be freed) on success, NULL on failure or if no backup needed.
 */
char *bx_backup_create_copy(const char *path, const struct bx_backup_params *params, struct bx_diag_ctx *diag);

#endif /* BX_COMMON_BACKUP_OPS_H */
