#ifndef BX_MODULE_PLAN_H
#define BX_MODULE_PLAN_H

#ifdef __linux__
#include "module_index.h"

struct bx_module_step {
    struct bx_module_path target;
    struct bx_module_path name;
    bool builtin;
    bool requested;
};

struct bx_module_plan {
    struct bx_module_step* steps;
    size_t count;
};

enum { BX_MODULE_PLAN_CYCLE = -2 };

/* Plan all matches of a name or installed alias. Caller frees steps; strings
 * are borrowed from tree. */
int bx_module_tree_plan(struct bx_module_tree* tree, const char* query, struct bx_module_plan* plan);
#endif

#endif
