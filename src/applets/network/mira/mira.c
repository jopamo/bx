#include "applets.h"
#include "github.h"
#include "mira.h"
#include "lib/fetch/exit_code.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static int mira_run_main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "github") == 0)
        return bx_mira_github_main(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "gitlab") == 0)
        return bx_mira_gitlab_main(argc - 1, argv + 1);

    struct bx_fetch_config* config = bx_mira_parse_cli(argc, argv);
    if (!config)
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;

    int result = BX_FETCH_EXIT_SUCCESS;
    if (config->startup.show_version)
        printf("mira %s\n", BX_VERSION);
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

int bx_mira_main(int argc, char** argv) {
    struct sigaction previous = {0};
    struct sigaction ignored = {
        .sa_handler = SIG_IGN,
    };
    sigemptyset(&ignored.sa_mask);
    if (sigaction(SIGPIPE, &ignored, &previous) != 0) {
        fprintf(stderr, "mira: cannot configure pipe handling: %s\n", strerror(errno));
        return BX_FETCH_EXIT_FILE_IO;
    }

    int result = mira_run_main(argc, argv);
    if (sigaction(SIGPIPE, &previous, NULL) != 0 && result == BX_FETCH_EXIT_SUCCESS) {
        fprintf(stderr, "mira: cannot restore pipe handling: %s\n", strerror(errno));
        result = BX_FETCH_EXIT_FILE_IO;
    }
    return result;
}
