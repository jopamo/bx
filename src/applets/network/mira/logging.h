#ifndef BX_APPLETS_NETWORK_MIRA_LOGGING_H
#define BX_APPLETS_NETWORK_MIRA_LOGGING_H

#include "lib/fetch/config.h"
#include <stdio.h>

/*
 * Opens the applet-owned diagnostic destination. Dry-run compatibility keeps
 * diagnostics on stderr without touching the configured log path.
 */
FILE* bx_mira_diagnostics_open(const struct bx_fetch_config* config);

/* Closes an owned destination and folds delayed write failure into exit_code. */
int bx_mira_diagnostics_finish(const struct bx_fetch_config* config, FILE* diagnostics, int exit_code);

#endif
