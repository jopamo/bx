#include <stdbool.h>
#include <sys/stat.h>

#include "stat_ops.h"
#include "update_policy.h"

bool bx_update_should_skip(enum bx_update_mode mode,
                           const struct stat *src_stat,
                           const struct stat *dest_stat,
                           bool *skip_out,
                           bool *error_out) {
    *skip_out = false;
    *error_out = false;

    switch (mode) {
    case BX_UPDATE_ALL:
        return true;
    case BX_UPDATE_NONE:
        *skip_out = true;
        return true;
    case BX_UPDATE_NONE_FAIL:
        *error_out = true;
        return false;
    case BX_UPDATE_OLDER:
        if (bx_stat_timespec_compare(&src_stat->st_mtim, &dest_stat->st_mtim) <= 0) {
            *skip_out = true;
        }
        return true;
    }

    return true;
}
