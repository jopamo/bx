#ifndef BX_COMMON_BACKUP_OPS_H
#define BX_COMMON_BACKUP_OPS_H

#include <sys/stat.h>
#include "args_common.h"
#include "bx/diag.h"

struct bx_backup_params {
    enum bx_backup_mode mode;
    const char* suffix;
};

enum bx_backup_create_result {
    BX_BACKUP_CREATE_SKIPPED = 0,
    BX_BACKUP_CREATE_CREATED,
    BX_BACKUP_CREATE_FAILED,
    BX_BACKUP_CREATE_SOURCE_CONFLICT,
};

/*
 * bx_backup_get_params: determine backup mode and suffix from args and environment.
 */
void bx_backup_get_params(enum bx_backup_mode cmd_mode, const char* cmd_suffix, struct bx_backup_params* out);

/*
 * bx_backup_create: attempt to rename 'path' to a backup name.
 * A non-NULL source_path/source_stat protects the selected source from the
 * backup rename. SOURCE_CONFLICT leaves both paths unchanged.
 * Stores the backup name in backup_path_out on success.
 */
enum bx_backup_create_result bx_backup_create(const char* path, const struct bx_backup_params* params, const char* source_path, const struct stat* source_stat, struct bx_diag_ctx* diag, char** backup_path_out);

/* Check destination ownership before unlinking; restore without replacing. */
bool bx_backup_restore(const char* backup_path, const char* dest_path, const struct stat* created_destination, struct bx_diag_ctx* diag);

/*
 * bx_backup_create_copy: attempt to copy 'path' to a backup name.
 * Returns the operation status and stores the backup name in backup_path_out on success.
 */
enum bx_backup_create_result bx_backup_create_copy(const char* path, const struct bx_backup_params* params, struct bx_diag_ctx* diag, char** backup_path_out);

#endif /* BX_COMMON_BACKUP_OPS_H */
