#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

struct bx_tty_options {
    const char* progname;
    bool silent;
    bool show_help;
    bool show_version;
};

static void bx_tty_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    fprintf(stream, "Print the file name of the terminal connected to standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -s, --silent, --quiet\n");
    fprintf(stream, "         print nothing, only return an exit status\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
}

static bool bx_tty_parse_options(int argc, char** argv, struct bx_tty_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"silent", no_argument, NULL, 's'}, {"quiet", no_argument, NULL, 's'}, {"help", no_argument, NULL, 1}, {"version", no_argument, NULL, 2}, {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "tty");
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+s", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 's':
                options->silent = true;
                break;
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

static int bx_tty_emit_state(const struct bx_tty_options* options, bool stdin_is_tty, const char* tty_name, struct bx_diag_ctx* diag) {
    if (options->silent) {
        return stdin_is_tty ? 0 : 1;
    }

    if (stdin_is_tty) {
        if (!bx_cli_emit_line(tty_name, false, diag)) {
            return diag->exit_status;
        }
    }
    else {
        if (!bx_cli_emit_line("not a tty", false, diag)) {
            return diag->exit_status;
        }
    }

    if (!bx_cli_flush_stdout(diag)) {
        return diag->exit_status;
    }

    return stdin_is_tty ? 0 : 1;
}

int bx_tty_main(int argc, char** argv) {
    struct bx_tty_options options;
    struct bx_diag_ctx diag = {
        .progname = "tty",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_tty_parse_options(argc, argv, &options, &first_operand, &diag)) {
        bx_cli_print_try_help(options.progname);
        return 2;
    }

    if (options.show_help) {
        bx_tty_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (first_operand < argc) {
        bx_cli_diag_extra_operand(&diag, argv[first_operand]);
        bx_cli_print_try_help(options.progname);
        return 2;
    }

    const char* tty_name = ttyname(STDIN_FILENO);
    bool stdin_is_tty = (tty_name != NULL);
    return bx_tty_emit_state(&options, stdin_is_tty, tty_name, &diag);
}
