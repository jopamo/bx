#ifndef BX_COMMON_OVERWRITE_POLICY_H
#define BX_COMMON_OVERWRITE_POLICY_H

#include <stdbool.h>
#include <sys/stat.h>

enum bx_overwrite_mode {
    BX_OVERWRITE_DEFAULT = 0,
    BX_OVERWRITE_INTERACTIVE,
    BX_OVERWRITE_FORCE,
    BX_OVERWRITE_NO_CLOBBER,
};

struct bx_overwrite_policy {
    enum bx_overwrite_mode mode;
    bool remove_destination;
};

#endif /* BX_COMMON_OVERWRITE_POLICY_H */
