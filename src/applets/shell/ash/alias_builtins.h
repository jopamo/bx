#ifndef BX_APPLETS_SHELL_ASH_ALIAS_BUILTINS_H
#define BX_APPLETS_SHELL_ASH_ALIAS_BUILTINS_H

struct ash_command;
struct ash_shell;

int ash_alias_builtin(
    struct ash_shell* shell,
    const struct ash_command* command
);
int ash_unalias_builtin(
    struct ash_shell* shell,
    const struct ash_command* command
);

#endif /* BX_APPLETS_SHELL_ASH_ALIAS_BUILTINS_H */
