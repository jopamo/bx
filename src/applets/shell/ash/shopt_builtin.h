#ifndef BX_APPLETS_SHELL_ASH_SHOPT_BUILTIN_H
#define BX_APPLETS_SHELL_ASH_SHOPT_BUILTIN_H

struct ash_command;
struct ash_shell;

int ash_shopt_builtin(
    struct ash_shell* shell,
    const struct ash_command* command
);

#endif /* BX_APPLETS_SHELL_ASH_SHOPT_BUILTIN_H */
