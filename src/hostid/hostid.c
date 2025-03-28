#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <string.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

static void bx_hostid_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]\n", progname);
    fprintf(stream, "Print the numeric identifier (in hexadecimal) for the current host.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_hostid_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

int bx_hostid_main(int argc, char** argv) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
        switch (c) {
            case 1:
                bx_hostid_print_help(stdout, "hostid");
                return 0;
            case 2:
                bx_hostid_print_version("hostid");
                return 0;
            default:
                return 1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "hostid: extra operand '%s'\n", argv[optind]);
        return 1;
    }

    long id = gethostid();
    printf("%08lx\n", (unsigned long)id & 0xffffffff);

    return 0;
}
