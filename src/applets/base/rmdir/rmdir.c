#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"

enum {
    BX_RMDIR_OPT_IGNORE_FAIL_ON_NON_EMPTY = 256,
};

struct bx_rmdir_options {
    const char* progname;
    bool parents;
    bool ignore_fail_on_non_empty;
    bool verbose;
    bool show_help;
    bool show_version;
};

static void bx_rmdir_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... DIRECTORY...\n", progname);
    fprintf(stream, "Remove the DIRECTORY(ies), if they are empty.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --ignore-fail-on-non-empty  ignore each failure that is solely because a directory is non-empty\n");
    fprintf(stream, "  -p, --parents                   remove DIRECTORY and its ancestors\n");
    fprintf(stream, "  -v, --verbose                   output a diagnostic for every directory processed\n");
    fprintf(stream, "      --help                      display this help and exit\n");
    fprintf(stream, "      --version                   output version information and exit\n");
}

static bool bx_rmdir_parse_options(int argc, char** argv, struct bx_rmdir_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"ignore-fail-on-non-empty", no_argument, NULL, BX_RMDIR_OPT_IGNORE_FAIL_ON_NON_EMPTY},
        {"parents", no_argument, NULL, 'p'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "rmdir");
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+pv", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case BX_RMDIR_OPT_IGNORE_FAIL_ON_NON_EMPTY:
                options->ignore_fail_on_non_empty = true;
                break;
            case 'p':
                options->parents = true;
                break;
            case 'v':
                options->verbose = true;
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

static bool bx_rmdir_is_non_empty_errno(int err) {
    return err == ENOTEMPTY || err == EEXIST;
}

static void bx_rmdir_strip_trailing_slashes(char* path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
}

static bool bx_rmdir_parent_path_inplace(char* path) {
    bx_rmdir_strip_trailing_slashes(path);

    char* slash = strrchr(path, '/');
    if (slash == NULL) {
        return false;
    }

    while (slash > path && slash[-1] == '/') {
        slash--;
    }

    if (slash == path) {
        return false;
    }

    *slash = '\0';
    bx_rmdir_strip_trailing_slashes(path);
    return true;
}

static bool bx_rmdir_print_verbose(const struct bx_rmdir_options* options, const char* path, struct bx_diag_ctx* diag) {
    if (!options->verbose) {
        return true;
    }

    if (printf("%s: removing directory, '%s'\n", options->progname, path) < 0) {
        int err = errno;
        errno = err;
        bx_diag(diag, "write error: %s", strerror(err));
        return false;
    }

    return true;
}

static bool bx_rmdir_remove_path(const char* path, const struct bx_rmdir_options* options, struct bx_diag_ctx* diag, bool* removed_out) {
    *removed_out = false;

    if (!bx_rmdir_print_verbose(options, path, diag)) {
        return false;
    }

    if (rmdir(path) == 0) {
        *removed_out = true;
        return true;
    }

    int err = errno;
    if (options->ignore_fail_on_non_empty && bx_rmdir_is_non_empty_errno(err)) {
        return true;
    }

    errno = err;
    bx_perror_path(diag, path);
    return false;
}

static void bx_rmdir_remove_operand(const char* operand, const struct bx_rmdir_options* options, struct bx_diag_ctx* diag) {
    if (!options->parents) {
        bool removed = false;
        (void)bx_rmdir_remove_path(operand, options, diag, &removed);
        return;
    }

    char* path = xstrdup(operand);
    bx_rmdir_strip_trailing_slashes(path);

    while (true) {
        bool removed = false;
        if (!bx_rmdir_remove_path(path, options, diag, &removed)) {
            break;
        }
        if (!removed) {
            break;
        }
        if (!bx_rmdir_parent_path_inplace(path)) {
            break;
        }
    }

    free(path);
}

int bx_rmdir_main(int argc, char** argv) {
    struct bx_rmdir_options options;
    struct bx_diag_ctx diag = {
        .progname = "rmdir",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_rmdir_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_rmdir_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_cli_diag_missing_operand(&diag);
        return diag.exit_status;
    }

    for (int i = first_operand; i < argc; i++) {
        bx_rmdir_remove_operand(argv[i], &options, &diag);
    }

    return diag.exit_status;
}
