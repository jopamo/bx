#ifndef BX_MODULE_ALIAS_H
#define BX_MODULE_ALIAS_H

#ifdef __linux__
#include "module_index.h"

struct bx_module_aliases {
    struct bx_module_alias_match* matches;
    size_t count;
};

enum { BX_MODULE_ALIAS_LIMIT = 1024 };
bool bx_module_alias_span_valid(const char* query, size_t len);
bool bx_module_alias_pattern_valid(struct bx_module_path pattern);
bool bx_module_alias_query_valid(const char* query);
/* Loadable modules precede built-ins; exact names precede aliases within each.
 * The caller frees matches, whose names borrow storage from tree or matches. */
int bx_module_resolve_alias(struct bx_module_tree* tree, const char* query,
                            struct bx_module_aliases* aliases);
struct bx_module_path bx_module_alias_at(const struct bx_module_aliases* aliases, size_t i);
#endif

#endif
