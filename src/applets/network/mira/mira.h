#ifndef BX_APPLETS_NETWORK_MIRA_H
#define BX_APPLETS_NETWORK_MIRA_H

#include "lib/fetch/config.h"

struct bx_fetch_config* bx_mira_parse_cli(int argc, char** argv);
void bx_mira_emit_parse_error(const struct bx_fetch_config* config, const char* summary);
void bx_mira_print_help(void);
int bx_mira_run_config(const struct bx_fetch_config* config);

#endif
