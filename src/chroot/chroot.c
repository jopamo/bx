#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "applets.h"
#include "diag.h"

struct bx_chroot_options {
    const char* progname;
    bool show_help;
    bool show_version;
};

static const char* bx_chroot_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "chroot";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_chroot_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s NEWROOT [COMMAND [ARG]...]\n", progname);
    fprintf(stream, "Run COMMAND with root directory set to NEWROOT.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_chroot_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_chroot_parse_options(int argc, char** argv, struct bx_chroot_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_chroot_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+", long_options, NULL);
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

int bx_chroot_main(int argc, char** argv) {
    struct bx_chroot_options options;
    struct bx_diag_ctx diag = {
        .progname = "chroot",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_chroot_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_chroot_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_chroot_print_version(options.progname);
        return 0;
    }

    if (first_operand >= argc) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    bx_diag(&diag, "chroot applet is not yet implemented");
    return diag.exit_status;
}
