#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/args_common.h"

struct bx_unlink_options {
    const char* progname;
    bool show_help;
    bool show_version;
};

static void bx_unlink_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s FILE\n", progname);
    fprintf(stream, "  or:  %s OPTION\n", progname);
    fprintf(stream, "Call the unlink function to remove the specified FILE.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool bx_unlink_parse_options(int argc, char** argv, struct bx_unlink_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "unlink");
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "+", long_options, &option_index);
        if (c == -1) {
            break;
        }

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

int bx_unlink_main(int argc, char** argv) {
    struct bx_unlink_options options;
    struct bx_diag_ctx diag = {
        .progname = "unlink",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_unlink_parse_options(argc, argv, &options, &first_operand, &diag)) {
        bx_cli_print_try_help(diag.progname);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_unlink_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    char** operands = argv + first_operand;
    if (operand_count <= 0) {
        bx_cli_diag_missing_operand(&diag);
        bx_cli_print_try_help(options.progname);
        return diag.exit_status;
    }

    if (operand_count > 1) {
        bx_cli_diag_extra_operand(&diag, operands[1]);
        bx_cli_print_try_help(options.progname);
        return diag.exit_status;
    }

    if (bx_fd_unlinkat(AT_FDCWD, operands[0], 0) != 0) {
        bx_diag(&diag, "cannot unlink '%s': %s", operands[0], strerror(errno));
    }

    return diag.exit_status;
}
