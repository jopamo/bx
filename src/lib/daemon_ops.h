#ifndef BX_LIB_DAEMON_OPS_H
#define BX_LIB_DAEMON_OPS_H

#include <stdbool.h>

/* Returns 0 in the daemon, 1 in the original parent, and -1 on failure. */
int bx_daemonize(bool chdir_root, bool preserve_stdout);

#endif
