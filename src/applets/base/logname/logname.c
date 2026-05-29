#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"

static void bx_logname_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]\n", progname);
    fprintf(stream, "Print the user's login name.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

struct bx_logname_options {
    const char* progname;
    bool show_help;
    bool show_version;
};

static bool bx_logname_parse_options(int argc,
                                     char** argv,
                                     struct bx_logname_options* options,
                                     int* first_operand,
                                     struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };
    int c;

    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "logname");
    options->show_help = false;
    options->show_version = false;
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while ((c = bx_args_getopt_long(argc, argv, "+", long_options, NULL)) != -1) {
        switch (c) {
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

int bx_logname_main(int argc, char** argv) {
    struct bx_logname_options options;
    struct bx_diag_ctx diag = {
        .progname = "logname",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_logname_parse_options(argc, argv, &options, &first_operand, &diag)) {
        bx_cli_print_try_help(diag.progname);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_logname_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (first_operand < argc) {
        bx_cli_diag_extra_operand(&diag, argv[first_operand]);
        bx_cli_print_try_help(options.progname);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    const char* login = getlogin();
    if (login == NULL) {
        bx_diag(&diag, "no login name");
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }
    printf("%s\n", login);

    return 0;
}
