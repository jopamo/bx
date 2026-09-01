#ifndef BX_APPLETS_SHELL_ASH_FUNCTIONS_H
#define BX_APPLETS_SHELL_ASH_FUNCTIONS_H

#include <stdbool.h>

struct ash_ast;
struct ash_shell;

struct ash_function {
    /* Context-owned node containing an owned name and deep-cloned AST body. */
    char* name;
    struct ash_ast* body;
    struct ash_function* next;
};

const struct ash_function* ash_function_find(
    const struct ash_shell* shell,
    const char* name
);
bool ash_function_define(
    struct ash_shell* shell,
    const char* name,
    const struct ash_ast* body
);
void ash_function_unset(struct ash_shell* shell, const char* name);
void ash_functions_destroy(struct ash_shell* shell);

#endif /* BX_APPLETS_SHELL_ASH_FUNCTIONS_H */
