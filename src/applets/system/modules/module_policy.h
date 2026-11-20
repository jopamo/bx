#ifndef BX_MODULE_POLICY_H
#define BX_MODULE_POLICY_H

#ifdef __linux__
#include "module_index.h"

struct bx_diag_ctx;
struct bx_module_rule;

struct bx_module_policy {
    struct bx_module_rule* rules;
    size_t count;
};

/* Null path selects default config directories. Always close the policy. */
int bx_module_policy_open(struct bx_module_policy* policy, const char* path, struct bx_diag_ctx* diag);
void bx_module_policy_close(struct bx_module_policy* policy);
bool bx_module_policy_blocks(const struct bx_module_policy* policy, struct bx_module_path name);
/* Caller frees the combined configured and command-line parameters. */
char* bx_module_policy_parameters(const struct bx_module_policy* policy, struct bx_module_path name,
                                  const char* cli);
#endif

#endif
