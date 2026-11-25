#ifndef BX_SYSTEM_CHOWN_H
#define BX_SYSTEM_CHOWN_H

#include "lib/cli_common.h"

int bx_chown_run(int argc, char** argv, bool group_only, bx_cli_help_fn print_help);

#endif /* BX_SYSTEM_CHOWN_H */
