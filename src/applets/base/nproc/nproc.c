#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"

struct bx_nproc_options {
    const char* progname;
    bool all;
    int ignore;
    bool show_help;
    bool show_version;
};

static void bx_nproc_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    fprintf(stream, "Print the number of processing units available to the current process,\n");
    fprintf(stream, "which may be less than the number of online processors.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --all      print the number of installed processors\n");
    fprintf(stream, "      --ignore=N  if possible, exclude N processors\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_nproc_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_nproc_parse_options(int argc, char** argv, struct bx_nproc_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"all", no_argument, NULL, 1}, {"ignore", required_argument, NULL, 2}, {"help", no_argument, NULL, 3}, {"version", no_argument, NULL, 4}, {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "nproc";
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 1:
                options->all = true;
                break;
            case 2:
                options->ignore = atoi(optarg);
                break;
            case 3:
                options->show_help = true;
                return true;
            case 4:
                options->show_version = true;
                return true;
            case '?':
                bx_diag(diag, "invalid option -- '%c'", optopt);
                return false;
            default:
                return false;
        }
    }

    return true;
}

int bx_nproc_main(int argc, char** argv) {
    struct bx_nproc_options options;
    struct bx_diag_ctx diag = {.progname = "nproc", .exit_status = 0};

    if (!bx_nproc_parse_options(argc, argv, &options, &diag))
        return 1;
    if (options.show_help) {
        bx_nproc_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_nproc_print_version(options.progname);
        return 0;
    }

    long nproc;
    if (options.all) {
        nproc = sysconf(_SC_NPROCESSORS_CONF);
    }
    else {
        nproc = sysconf(_SC_NPROCESSORS_ONLN);
    }

    if (nproc <= 0)
        nproc = 1;

    nproc -= options.ignore;
    if (nproc <= 0)
        nproc = 1;

    printf("%ld\n", nproc);

    return 0;
}
