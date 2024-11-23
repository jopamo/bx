#ifndef BX_COMMON_OVERWRITE_OPS_H
#define BX_COMMON_OVERWRITE_OPS_H

#include <stdbool.h>
#include <sys/stat.h>

#include "backup_ops.h"
#include "diag.h"
#include "stat_ops.h"
#include "update_policy.h"

enum bx_overwrite_skip_reason {
    BX_OVERWRITE_SKIP_NONE = 0,
    BX_OVERWRITE_SKIP_NO_CLOBBER,
    BX_OVERWRITE_SKIP_UPDATE,
};

bool bx_overwrite_should_skip(bool no_clobber,
                              bool interactive,
                              enum bx_update_mode update_mode,
                              const char* dest_path,
                              const struct stat* src_stat,
                              const struct stat* dest_stat,
                              bool* skip_out,
                              enum bx_overwrite_skip_reason* reason_out,
                              struct bx_diag_ctx* diag);

bool bx_overwrite_backup_existing(const char* dest_path, const struct bx_backup_params* backup_params, struct bx_diag_ctx* diag, struct bx_dest_state* dest_state, char** backup_path_out);

bool bx_prompt_overwrite(const char* progname, const char* dest_path);

#endif /* BX_COMMON_OVERWRITE_OPS_H */
