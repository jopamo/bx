#ifndef BX_COMMON_REMOVE_OPS_H
#define BX_COMMON_REMOVE_OPS_H

#include <stdbool.h>
#include <sys/types.h>
#include "diag.h"

bool bx_remove_recursive(const char* path, struct bx_diag_ctx* diag);
bool bx_remove_recursive_one_file_system(const char* path, dev_t root_dev, struct bx_diag_ctx* diag);

#endif /* BX_COMMON_REMOVE_OPS_H */
