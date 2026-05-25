#ifndef BX_LIB_MANUAL_RUNTIME_H
#define BX_LIB_MANUAL_RUNTIME_H

#include <stdio.h>

#include "lib/cli_common.h"

int bx_manual_runtime_main(
    int argc,
    char **argv,
    const char *fallback_progname,
    bx_cli_help_fn print_help
);

#endif
