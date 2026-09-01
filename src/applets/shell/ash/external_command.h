#ifndef BX_APPLETS_SHELL_ASH_EXTERNAL_COMMAND_H
#define BX_APPLETS_SHELL_ASH_EXTERNAL_COMMAND_H

struct ash_command_resolution;
struct ash_shell;

/*
 * Execute one external resolution in the current process. argv and the
 * resolution borrow caller-owned storage. A successful exec does not return.
 */
int ash_external_command_exec(
    struct ash_shell* shell,
    char** argv,
    const struct ash_command_resolution* resolution
);

#endif /* BX_APPLETS_SHELL_ASH_EXTERNAL_COMMAND_H */
