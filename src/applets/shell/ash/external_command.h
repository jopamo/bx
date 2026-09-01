#ifndef BX_APPLETS_SHELL_ASH_EXTERNAL_COMMAND_H
#define BX_APPLETS_SHELL_ASH_EXTERNAL_COMMAND_H

struct ash_command_resolution;
struct ash_shell;

/*
 * Execute one already resolved pathname without PATH search.
 */
int ash_external_command_exec_exact(
    struct ash_shell* shell,
    const char* command_name,
    const char* executable,
    char** argv
);
/*
 * Execute one already-open native image without pathname rediscovery.
 */
int ash_external_command_exec_fd_exact(
    struct ash_shell* shell,
    const char* command_name,
    int executable_fd,
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
