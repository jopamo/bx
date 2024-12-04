#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "applets.h"
#include "diag.h"

struct bx_mkfifo_options {
    const char* progname;
    bool show_help;
    bool show_version;
};

static const char* bx_mkfifo_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "mkfifo";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_mkfifo_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... NAME...\n", progname);
    fprintf(stream, "Create the FIFO special files NAMEs.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_mkfifo_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_mkfifo_parse_options(int argc, char** argv, struct bx_mkfifo_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_mkfifo_progname((argc > 0) ? argv[0] : NULL);
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

int bx_mkfifo_main(int argc, char** argv) {
    struct bx_mkfifo_options options;
    struct bx_diag_ctx diag = {
        .progname = "mkfifo",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_mkfifo_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_mkfifo_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_mkfifo_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    for (int i = first_operand; i < argc; i++) {
        if (mkfifo(argv[i], 0666u) != 0) {
            bx_perror_path(&diag, argv[i]);
        }
    }

    return diag.exit_status;
}
