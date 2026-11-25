#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/mode_parse.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/option_scan.h"

enum {
    BX_MKFIFO_OPT_HELP = 256,
    BX_MKFIFO_OPT_VERSION,
};

struct bx_mkfifo_options {
    const char* progname;
    bool mode_set;
    mode_t mode;
    bool show_help;
    bool show_version;
    const char** paths;
    size_t path_count;
};

static void bx_mkfifo_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... NAME...\n", progname);
    fprintf(stream, "Create the FIFO special files NAMEs.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Mandatory arguments to long options are mandatory for short options too.\n");
    fprintf(stream, "  -m, --mode=MODE  set file permission bits to MODE, not a=rw - umask\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool bx_mkfifo_parse_mode(const char* text, mode_t* mode_out, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0') {
        bx_diag(diag, "invalid mode '%s'", (text != NULL) ? text : "");
        return false;
    }

    struct bx_mode_parse_params params = {
        .initial_mode = 0666u,
        .result_mask = 0777u,
        .max_numeric_mode = 0777u,
        .umask_value = bx_mode_current_umask(),
        .sticky_bit = 0u,
        .x_policy = BX_MODE_X_IF_ANY_EXEC,
        .is_directory = false,
        .apply_umask_when_who_omitted = true,
        .allow_setuid = false,
        .allow_setgid = false,
        .allow_sticky = false,
    };

    if (bx_mode_parse(text, &params, mode_out)) {
        return true;
    }

    bx_diag(diag, "invalid mode '%s'", text);
    return false;
}

static bool bx_mkfifo_parse_options(int argc, char** argv, struct bx_mkfifo_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'m', 'm', "mode", BX_OPTION_REQUIRED_VALUE},
        {BX_MKFIFO_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_MKFIFO_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
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
            if (result.event == BX_OPTION_MISSING_VALUE) {
                if (result.short_name != 0)
                    bx_diag(diag, "option requires an argument -- '%c'", result.short_name);
                else
                    bx_diag(diag, "option requires an argument -- '%s'", result.token);
            }
            else if (result.short_name != 0)
                bx_diag(diag, "invalid option -- '%c'", result.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", result.token);
            return false;
        }

        switch (result.spec->id) {
            case 'm':
                if (!bx_mkfifo_parse_mode(result.value, &options->mode, diag)) {
                    return false;
                }
                options->mode_set = true;
                break;
            case BX_MKFIFO_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_MKFIFO_OPT_VERSION:
                options->show_version = true;
                return true;
            default:
                return false;
        }
    }
}

static int bx_mkfifo_create_fifo(const char* path, mode_t mode, bool explicit_mode) {
    if (!explicit_mode) {
        return bx_fd_mkfifoat(AT_FDCWD, path, mode);
    }

    mode_t old_umask = umask(0u);
    int rc = bx_fd_mkfifoat(AT_FDCWD, path, mode | S_IRUSR);
    int saved_errno = errno;
    umask(old_umask);
    errno = saved_errno;
    return rc;
}

static bool bx_mkfifo_chmod_created_fifo(const char* path, mode_t mode, struct bx_diag_ctx* diag) {
    int fd = bx_fd_open_nofollow_cloexec(path, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0) {
        bx_perror_path(diag, path);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        bx_perror_path(diag, path);
        bx_fd_cleanup(&fd);
        return false;
    }

    if (!S_ISFIFO(st.st_mode)) {
        errno = EEXIST;
        bx_perror_path(diag, path);
        bx_fd_cleanup(&fd);
        return false;
    }

    if (bx_fd_fchmod(fd, mode) != 0) {
        bx_perror_path(diag, path);
        bx_fd_cleanup(&fd);
        return false;
    }

    return bx_fd_close(&fd, path, diag);
}

static int bx_mkfifo_run(const struct bx_mkfifo_options* options, struct bx_diag_ctx* diag) {
    if (options->show_help) {
        bx_mkfifo_print_help(stdout, options->progname);
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
    mode_t create_mode = options->mode_set ? options->mode : 0666u;
    for (size_t i = 0; i < options->path_count; i++) {
        const char* path = options->paths[i];
        if (bx_mkfifo_create_fifo(path, create_mode, options->mode_set) != 0) {
            bx_perror_path(diag, path);
        }
        else if (options->mode_set) {
            (void)bx_mkfifo_chmod_created_fifo(path, options->mode, diag);
        }
    }
    return diag->exit_status;
}

int bx_mkfifo_main(int argc, char** argv) {
    struct bx_mkfifo_options options = {
        .progname = bx_cli_progname(argc > 0 ? argv[0] : NULL, "mkfifo"),
    };
    struct bx_diag_ctx diag = {.progname = options.progname};
    int status;
    if (bx_mkfifo_parse_options(argc, argv, &options, &diag))
        status = bx_mkfifo_run(&options, &diag);
    else
        status = diag.exit_status != 0 ? diag.exit_status : 1;
    free(options.paths);
    return status;
}
