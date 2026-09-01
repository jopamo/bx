#ifndef BX_SELF_EXEC_H
#define BX_SELF_EXEC_H

#include <stdbool.h>

/*
 * Process-wide immutable authority for re-executing the current bx binary.
 *
 * Initialization verifies and opens the executable before an applet that
 * requires self-dispatch enters. Verification binds the candidate descriptor
 * to the kernel's loaded-image metadata and linker build ID. Consumers receive
 * owned CLOEXEC descriptor duplicates or path copies; they never rediscover
 * identity through argv[0], PATH, or procfs.
 */
bool bx_self_exec_initialize(const char* argv0);
void bx_self_exec_discard_handoff(void);
int bx_self_exec_fd_dup(void);
char* bx_self_exec_path_dup(void);

#endif /* BX_SELF_EXEC_H */
