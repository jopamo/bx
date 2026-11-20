#ifndef BX_MODULE_PLAN_H
#define BX_MODULE_PLAN_H

#ifdef __linux__
#include "module_index.h"

struct bx_module_plan {
    struct bx_module_path* files;
    size_t count;
    bool builtin;
};

enum { BX_MODULE_PLAN_CYCLE = -2 };

/* On success the caller frees files, whose strings are borrowed from tree. */
int bx_module_tree_plan(struct bx_module_tree* tree, const char* name, struct bx_module_plan* plan);
#endif

#endif
