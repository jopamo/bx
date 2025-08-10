#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"

struct bx_link_options {
    const char* progname;
    bool show_help;
    bool show_version;
};

static const char* bx_link_progname(const char* argv0) {
    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    if (argv0 != NULL && argv0[0] != '\0') {
        return argv0;
    }
    return "link";
}

static void bx_link_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s FILE1 FILE2\n", progname);
    fprintf(stream, "  or:  %s OPTION\n", progname);
    fprintf(stream, "Call the link function to create a link named FILE2 to an existing FILE1.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
}

static void bx_link_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void bx_link_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static bool bx_link_parse_options(int argc, char** argv, struct bx_link_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_link_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+", long_options, &option_index);
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
                if (optopt != 0) {
                    bx_diag(diag, "invalid option -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "unrecognized option");
                }
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

int bx_link_main(int argc, char** argv) {
    struct bx_link_options options;
    struct bx_diag_ctx diag = {
        .progname = "link",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_link_parse_options(argc, argv, &options, &first_operand, &diag)) {
        bx_link_print_try_help(diag.progname);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_link_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_link_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    char** operands = argv + first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        bx_link_print_try_help(options.progname);
        return diag.exit_status;
    }

    if (operand_count == 1) {
        bx_diag(&diag, "missing operand after '%s'", operands[0]);
        bx_link_print_try_help(options.progname);
        return diag.exit_status;
    }

    if (operand_count > 2) {
        bx_diag(&diag, "extra operand '%s'", operands[2]);
        bx_link_print_try_help(options.progname);
        return diag.exit_status;
    }

    if (link(operands[0], operands[1]) != 0) {
        bx_diag(&diag, "cannot create link '%s' to '%s': %s", operands[1], operands[0], strerror(errno));
    }

    return diag.exit_status;
}
