#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"

struct bx_setsid_options {
    const char* progname;
    bool show_help;
    bool show_version;
    int command_index;
};

static void bx_setsid_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... COMMAND [ARG]...\n", progname);
    fprintf(stream, "Run COMMAND in a new session.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -h, --help     display this help and exit\n");
    fprintf(stream, "  -V, --version  output version information and exit\n");
}

static bool bx_setsid_parse_options(int argc, char** argv, struct bx_setsid_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "setsid");
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int c = bx_args_getopt_long(argc, argv, "+hV", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'h':
                options->show_help = true;
                return true;
            case 'V':
                options->show_version = true;
                return true;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    options->command_index = optind;
    return true;
}

int bx_setsid_main(int argc, char** argv) {
    struct bx_setsid_options options;
    struct bx_diag_ctx diag = {
        .progname = "setsid",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_setsid_parse_options(argc, argv, &options, &diag)) {
        bx_cli_print_try_help(options.progname);
        return 1;
    }

    if (options.show_help) {
        bx_setsid_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (options.command_index >= argc) {
        bx_diag(&diag, "missing command operand");
        bx_cli_print_try_help(options.progname);
        return 1;
    }

    if (setsid() == -1) {
        bx_diag(&diag, "failed to create new session: %s", strerror(errno));
        return 1;
    }

    execvp(argv[options.command_index], argv + options.command_index);

    int exec_error = errno;
    bx_diag(&diag, "failed to run command '%s': %s", argv[options.command_index], strerror(exec_error));
    if (exec_error == ENOENT) {
        return 127;
    }
    return 126;
}
