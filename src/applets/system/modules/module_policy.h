#ifndef BX_MODULE_POLICY_H
#define BX_MODULE_POLICY_H

#ifdef __linux__
#include "module_index.h"

struct bx_diag_ctx;

struct bx_module_policy {
    char* config;
    char* cmdline;
    struct bx_module_path* blocked;
    size_t count;
};

/* Initialize an empty policy, and close it on either success or failure. */
int bx_module_policy_open(struct bx_module_policy* policy, const char* path, struct bx_diag_ctx* diag);
void bx_module_policy_close(struct bx_module_policy* policy);
bool bx_module_policy_blocks(const struct bx_module_policy* policy, struct bx_module_path name);
#endif

#endif
