#ifndef BX_APPLETS_BASE_XARGS_EXEC_H
#define BX_APPLETS_BASE_XARGS_EXEC_H

#include <signal.h>
#include <stdio.h>

#include "xargs_input.h"

int xargs_run_batches(const char *progname, char **command, int command_argc,
                      struct xargs_items *items, struct xargs_opts *opts,
                      volatile sig_atomic_t *interrupt_signal);
int xargs_run_streaming_batches(const char *progname, char **command,
                                int command_argc, FILE *input,
                                struct xargs_opts *opts,
                                volatile sig_atomic_t *interrupt_signal);

#endif
