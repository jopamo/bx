#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"

static void bx_logname_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]\n", progname);
    fprintf(stream, "Print the user's login name.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_logname_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

int bx_logname_main(int argc, char** argv) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
        switch (c) {
            case 1:
                bx_logname_print_help(stdout, "logname");
                return 0;
            case 2:
                bx_logname_print_version("logname");
                return 0;
            default:
                return 1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "logname: extra operand '%s'\n", argv[optind]);
        return 1;
    }

    char* login = getlogin();
    if (!login) {
        bx_pfatal(1, "no login name");
    }
    printf("%s\n", login);

    return 0;
}
