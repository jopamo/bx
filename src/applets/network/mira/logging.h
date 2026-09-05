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

/*
 * Opens the optional append-only rejected-URL destination. A NULL output is a
 * successful disabled/dry-run result; non-NULL streams are owned by the caller.
 */
int bx_mira_rejected_log_open(const struct bx_fetch_config* config, FILE** rejected_log_out);

/* Closes the optional destination and folds delayed write failure into exit. */
int bx_mira_rejected_log_finish(const struct bx_fetch_config* config, FILE* rejected_log, int exit_code);

#endif
