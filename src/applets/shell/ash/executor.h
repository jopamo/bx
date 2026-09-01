#ifndef BX_APPLETS_SHELL_ASH_EXECUTOR_H
#define BX_APPLETS_SHELL_ASH_EXECUTOR_H

struct ash_ast;
struct ash_shell;

int ash_execute_ast(
    struct ash_shell* shell,
    const struct ash_ast* node
);

#endif /* BX_APPLETS_SHELL_ASH_EXECUTOR_H */
