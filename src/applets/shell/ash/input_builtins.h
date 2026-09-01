#ifndef BX_APPLETS_SHELL_ASH_INPUT_BUILTINS_H
#define BX_APPLETS_SHELL_ASH_INPUT_BUILTINS_H

struct ash_command;
struct ash_shell;

int ash_input_builtin_eval(
    struct ash_shell* shell,
    const struct ash_command* command
);
int ash_input_builtin_source(
    struct ash_shell* shell,
    const struct ash_command* command
);

#endif /* BX_APPLETS_SHELL_ASH_INPUT_BUILTINS_H */
