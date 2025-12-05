#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void bx_hostname_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [NAME]\n", progname);
    fprintf(stream, "  or:  %s OPTION\n", progname);
    fprintf(stream, "Print or set the current system hostname.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With NAME, set the hostname to NAME.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static int bx_hostname_print_current(struct bx_diag_ctx* diag) {
    struct utsname uts;

    if (uname(&uts) != 0) {
        int saved_errno = errno;
        fprintf(stderr, "%s: cannot determine hostname: %s\n", diag->progname, strerror(saved_errno));
        return 1;
    }

    if (!bx_cli_emit_line(uts.nodename, false, diag)) {
        return diag->exit_status;
    }

    if (!bx_cli_flush_stdout(diag)) {
        return diag->exit_status;
    }

    return 0;
}

static int bx_hostname_set_name(const char* name, struct bx_diag_ctx* diag) {
    if (sethostname(name, strlen(name)) != 0) {
        int saved_errno = errno;
        fprintf(stderr, "%s: cannot set name to '%s': %s\n", diag->progname, name, strerror(saved_errno));
        return 1;
    }

    return 0;
}

int bx_hostname_main(int argc, char** argv) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "hostname"),
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 1:
                bx_hostname_print_help(stdout, diag.progname);
                return 0;
            case 2:
                bx_cli_print_version(diag.progname);
                return 0;
            case '?':
                bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
                bx_cli_print_try_help(diag.progname);
                return diag.exit_status;
            default:
                return 1;
        }
    }

    if (optind + 1 < argc) {
        bx_cli_diag_extra_operand(&diag, argv[optind + 1]);
        bx_cli_print_try_help(diag.progname);
        return diag.exit_status;
    }

    if (optind < argc) {
        return bx_hostname_set_name(argv[optind], &diag);
    }

    return bx_hostname_print_current(&diag);
}
