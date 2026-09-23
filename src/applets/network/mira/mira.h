#ifndef BX_APPLETS_NETWORK_MIRA_H
#define BX_APPLETS_NETWORK_MIRA_H

#include "lib/fetch/config.h"
#include "lib/fetch/net.h"

/* Mira defaults; provider presets override only command-specific policy. */
struct bx_fetch_config* bx_mira_config_new(void);
int bx_mira_apply_read_preset(struct bx_fetch_config* config);
struct bx_fetch_config* bx_mira_parse_cli(int argc, char** argv);
void bx_mira_emit_parse_error(const struct bx_fetch_config* config, const char* summary);
void bx_mira_print_help(void);
void bx_mira_print_read_help(void);
int bx_mira_run_config(const struct bx_fetch_config* config);
int bx_mira_run_config_with_budget(const struct bx_fetch_config* config, BxFetchBudget* budget);
int bx_mira_gitlab_main(int argc, char** argv);

#endif
