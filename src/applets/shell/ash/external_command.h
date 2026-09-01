#ifndef BX_APPLETS_SHELL_ASH_EXTERNAL_COMMAND_H
#define BX_APPLETS_SHELL_ASH_EXTERNAL_COMMAND_H

struct ash_command_resolution;
struct ash_shell;

/*
 * Execute one already resolved executable without PATH search. This is also
 * the canonical exact-exec fallback for an ineligible bx applet target.
 */
int ash_external_command_exec_exact(
    struct ash_shell* shell,
    const char* command_name,
    const char* executable,
    char** argv
);

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
