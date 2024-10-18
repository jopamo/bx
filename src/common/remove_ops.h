#ifndef BX_COMMON_REMOVE_OPS_H
#define BX_COMMON_REMOVE_OPS_H

#include <stdbool.h>
#include "diag.h"

bool bx_remove_recursive(const char *path, struct bx_diag_ctx *diag);

#endif /* BX_COMMON_REMOVE_OPS_H */
