#ifndef BX_APPLETS_SHELL_ASH_TRAP_BUILTIN_H
#define BX_APPLETS_SHELL_ASH_TRAP_BUILTIN_H

struct ash_command;
struct ash_shell;

int ash_trap_builtin(struct ash_shell* shell, const struct ash_command* command);

#endif
