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

enum { BX_MODULE_PLAN_CYCLE = -2, BX_MODULE_PLAN_NOTFOUND = -3 };

/* Plan all matches of the requested names or aliases. Caller frees steps;
 * strings are borrowed from tree. failed identifies a failed lookup, if any. */
int bx_module_tree_plan(struct bx_module_tree* tree, int nqueries, char* const* queries,
                        struct bx_module_plan* plan, const char** failed);
/* Plan one exact name and its hard dependencies, without opening module files. */
int bx_module_tree_remove_plan(struct bx_module_tree* tree, const char* name, struct bx_module_plan* plan);
#endif

#endif
