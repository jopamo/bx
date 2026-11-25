#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/option_scan.h"

enum {
    BX_RMDIR_OPT_IGNORE_FAIL_ON_NON_EMPTY = 256,
    BX_RMDIR_OPT_HELP,
    BX_RMDIR_OPT_VERSION,
};

struct bx_rmdir_options {
    const char* progname;
    bool parents;
    bool ignore_fail_on_non_empty;
    bool verbose;
    bool show_help;
    bool show_version;
    const char** paths;
    size_t path_count;
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

static bool bx_rmdir_parse_options(int argc, char** argv, struct bx_rmdir_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {BX_RMDIR_OPT_IGNORE_FAIL_ON_NON_EMPTY, 0, "ignore-fail-on-non-empty", BX_OPTION_NO_VALUE},
        {'p', 'p', "parents", BX_OPTION_NO_VALUE},
        {'v', 'v', "verbose", BX_OPTION_NO_VALUE},
        {BX_RMDIR_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_RMDIR_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };

    struct bx_option_cursor cursor;
    size_t end = argc > 0 ? (size_t)argc : 0;
    bx_option_cursor_init(&cursor, argv, end != 0, end, BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END)
            return true;
        if (result.event == BX_OPTION_OPERAND) {
            if (options->paths == NULL) {
                size_t capacity = end - 1;
                if (capacity > SIZE_MAX / sizeof(*options->paths) ||
                    (options->paths = malloc(capacity * sizeof(*options->paths))) == NULL) {
                    bx_diag(diag, "memory exhausted");
                    return false;
                }
            }
            options->paths[options->path_count++] = result.token;
            continue;
        }
        if (result.event != BX_OPTION_MATCH) {
            if (result.short_name != 0)
                bx_diag(diag, "invalid option -- '%c'", result.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", result.token);
            return false;
        }

        switch (result.spec->id) {
            case BX_RMDIR_OPT_IGNORE_FAIL_ON_NON_EMPTY:
                options->ignore_fail_on_non_empty = true;
                break;
            case 'p':
                options->parents = true;
                break;
            case 'v':
                options->verbose = true;
                break;
            case BX_RMDIR_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_RMDIR_OPT_VERSION:
                options->show_version = true;
                return true;
            default:
                return false;
        }
    }
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

    if (bx_fd_unlinkat(AT_FDCWD, path, AT_REMOVEDIR) == 0) {
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

static int bx_rmdir_run(const struct bx_rmdir_options* options, struct bx_diag_ctx* diag) {
    if (options->show_help) {
        bx_rmdir_print_help(stdout, options->progname);
        return 0;
    }
    if (options->show_version) {
        bx_cli_print_version(options->progname);
        return 0;
    }
    if (options->path_count == 0) {
        bx_cli_diag_missing_operand(diag);
        return diag->exit_status;
    }
    for (size_t i = 0; i < options->path_count; i++) {
        bx_rmdir_remove_operand(options->paths[i], options, diag);
    }
    return diag->exit_status;
}

int bx_rmdir_main(int argc, char** argv) {
    struct bx_rmdir_options options = {
        .progname = bx_cli_progname(argc > 0 ? argv[0] : NULL, "rmdir"),
    };
    struct bx_diag_ctx diag = {.progname = options.progname};
    int status;
    if (bx_rmdir_parse_options(argc, argv, &options, &diag))
        status = bx_rmdir_run(&options, &diag);
    else
        status = diag.exit_status != 0 ? diag.exit_status : 1;
    free(options.paths);
    return status;
}
