#ifndef BX_COMMON_UPDATE_POLICY_H
#define BX_COMMON_UPDATE_POLICY_H

#include <stdbool.h>
#include <sys/stat.h>

enum bx_update_mode {
    BX_UPDATE_ALL = 0,
    BX_UPDATE_NONE,
    BX_UPDATE_NONE_FAIL,
    BX_UPDATE_OLDER,
};

bool bx_update_should_skip(enum bx_update_mode mode, const struct stat* src_stat, const struct stat* dest_stat, bool* skip_out, bool* error_out);

#endif /* BX_COMMON_UPDATE_POLICY_H */
