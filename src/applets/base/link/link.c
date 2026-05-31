#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/same_file.h"
#include "lib/args_common.h"

struct bx_link_options {
    const char* progname;
    bool show_help;
    bool show_version;
};

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

static bool bx_link_parse_options(int argc, char** argv, struct bx_link_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "link");
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

static void bx_link_diag_create_failure(struct bx_diag_ctx* diag, const char* source_path, const char* dest_path, int err) {
    bx_diag(diag, "cannot create link '%s' to '%s': %s", dest_path, source_path, strerror(err));
}

static bool bx_link_source_stat(const char* source_path, struct stat* source_stat_out, int* err_out) {
    if (lstat(source_path, source_stat_out) == 0) {
        return true;
    }
    *err_out = errno;
    return false;
}

static bool bx_link_destination_matches_source(const char* dest_path, const struct stat* expected_source_stat) {
    struct stat dest_stat;

    if (lstat(dest_path, &dest_stat) != 0) {
        return false;
    }
    return bx_same_file(expected_source_stat, &dest_stat);
}

static bool bx_link_create_verified(const char* source_path, const char* dest_path, struct bx_diag_ctx* diag) {
    struct stat expected_source_stat;
    int err = 0;

    if (!bx_link_source_stat(source_path, &expected_source_stat, &err)) {
        bx_link_diag_create_failure(diag, source_path, dest_path, err);
        return false;
    }

    if (bx_fd_linkat(AT_FDCWD, source_path, AT_FDCWD, dest_path, 0) != 0) {
        bx_link_diag_create_failure(diag, source_path, dest_path, errno);
        return false;
    }

    if (bx_link_destination_matches_source(dest_path, &expected_source_stat)) {
        return true;
    }

    bx_diag(diag, "source '%s' changed during link", source_path);
    if (bx_fd_unlinkat(AT_FDCWD, dest_path, 0) != 0 && errno != ENOENT) {
        bx_diag(diag, "cannot unlink '%s': %s", dest_path, strerror(errno));
    }
    return false;
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
        bx_cli_print_try_help(diag.progname);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_link_print_help(stdout, options.progname);
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

    if (operand_count == 1) {
        bx_cli_diag_missing_operand_after(&diag, operands[0]);
        bx_cli_print_try_help(options.progname);
        return diag.exit_status;
    }

    if (operand_count > 2) {
        bx_cli_diag_extra_operand(&diag, operands[2]);
        bx_cli_print_try_help(options.progname);
        return diag.exit_status;
    }

    (void)bx_link_create_verified(operands[0], operands[1], &diag);

    return diag.exit_status;
}
