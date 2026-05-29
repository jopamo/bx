#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"

struct bx_nproc_options {
    const char* progname;
    bool all;
    uintmax_t ignore;
    bool show_help;
    bool show_version;
    int first_operand;
};

enum bx_nproc_parse_status {
    BX_NPROC_PARSE_OK = 0,
    BX_NPROC_PARSE_ERROR = 1,
    BX_NPROC_PARSE_ERROR_TRY_HELP = 2,
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

static bool bx_nproc_parse_ignore(const char* text, uintmax_t* value_out, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0') {
        bx_diag(diag, "invalid number: '%s'", text == NULL ? "" : text);
        return false;
    }

    const char* digits = text;
    if (digits[0] == '+') {
        digits++;
    }
    else if (digits[0] == '-') {
        bx_diag(diag, "invalid number: '%s'", text);
        return false;
    }

    if (digits[0] == '\0') {
        bx_diag(diag, "invalid number: '%s'", text);
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t value = strtoumax(digits, &end, 10);
    if (end == digits || *end != '\0') {
        bx_diag(diag, "invalid number: '%s'", text);
        return false;
    }

    if (errno == ERANGE) {
        value = UINTMAX_MAX;
    }

    *value_out = value;
    return true;
}

static enum bx_nproc_parse_status bx_nproc_parse_options(
    int argc,
    char** argv,
    struct bx_nproc_options* options,
    struct bx_diag_ctx* diag
) {
    static const struct option long_options[] = {
        {"all", no_argument, NULL, 1}, {"ignore", required_argument, NULL, 2}, {"help", no_argument, NULL, 3}, {"version", no_argument, NULL, 4}, {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "nproc");
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 1:
                options->all = true;
                break;
            case 2:
                if (!bx_nproc_parse_ignore(optarg, &options->ignore, diag)) {
                    return BX_NPROC_PARSE_ERROR;
                }
                break;
            case 3:
                options->show_help = true;
                return BX_NPROC_PARSE_OK;
            case 4:
                options->show_version = true;
                return BX_NPROC_PARSE_OK;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return BX_NPROC_PARSE_ERROR_TRY_HELP;
            default:
                return BX_NPROC_PARSE_ERROR;
        }
    }

    options->first_operand = optind;
    if (optind < argc) {
        bx_cli_diag_extra_operand(diag, argv[optind]);
        return BX_NPROC_PARSE_ERROR_TRY_HELP;
    }

    return BX_NPROC_PARSE_OK;
}

int bx_nproc_main(int argc, char** argv) {
    struct bx_nproc_options options;
    struct bx_diag_ctx diag = {.progname = "nproc", .exit_status = 0};
    enum bx_nproc_parse_status parse_status = bx_nproc_parse_options(argc, argv, &options, &diag);

    if (parse_status != BX_NPROC_PARSE_OK) {
        if (parse_status == BX_NPROC_PARSE_ERROR_TRY_HELP) {
            bx_cli_print_try_help(options.progname);
        }
        return 1;
    }
    if (options.show_help) {
        bx_nproc_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
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

    if (options.ignore >= (uintmax_t)nproc) {
        nproc = 1;
    }
    else {
        nproc -= (long)options.ignore;
    }
    if (nproc <= 0) {
        nproc = 1;
    }

    printf("%ld\n", nproc);

    return 0;
}
