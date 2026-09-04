#include "applets.h"
#include "mira.h"
#include "lib/fetch/exit_code.h"
#include <stdio.h>

#define BX_MIRA_VERSION "0.1.0"

int bx_mira_main(int argc, char** argv) {
    struct bx_fetch_config* config = bx_mira_parse_cli(argc, argv);
    if (!config)
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;

    int result = BX_FETCH_EXIT_SUCCESS;
    if (config->startup.show_version)
        printf("mira %s\n", BX_MIRA_VERSION);
    else if (config->startup.show_help)
        bx_mira_print_help();
    else if (config->input.url_count == 0 && !config->input.input_file) {
        bx_mira_emit_parse_error(config, "no URLs specified");
        result = BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    else
        result = bx_mira_run_config(config);

    bx_fetch_config_free(config);
    return result;
}
