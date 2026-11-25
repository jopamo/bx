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
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/mode_parse.h"
#include "lib/option_scan.h"

#ifdef S_ISVTX
#define BX_MKDIR_STICKY_BIT S_ISVTX
#else
#define BX_MKDIR_STICKY_BIT 01000
#endif

enum {
    BX_MKDIR_OPT_HELP = 256,
    BX_MKDIR_OPT_VERSION,
};

struct bx_mkdir_options {
    const char* progname;
    bool mode_set;
    mode_t mode;
    bool parents;
    bool verbose;
    bool show_help;
    bool show_version;
    const char** paths;
    size_t path_count;
};

static void bx_mkdir_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... DIRECTORY...\n", progname);
    fprintf(stream, "Create the DIRECTORY(ies), if they do not already exist.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Mandatory arguments to long options are mandatory for short options too.\n");
    fprintf(stream, "  -m, --mode=MODE\n");
    fprintf(stream, "         set file mode (as in chmod), not a=rwx - umask\n");
    fprintf(stream, "  -p, --parents\n");
    fprintf(stream, "         no error if existing, make parent directories as needed,\n");
    fprintf(stream, "         with their file modes unaffected by any -m option\n");
    fprintf(stream, "  -v, --verbose\n");
    fprintf(stream, "         print a message for each created directory\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
}

static bool bx_mkdir_parse_mode(const char* text, mode_t* mode_out, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0') {
        bx_diag(diag, "invalid mode '%s'", (text != NULL) ? text : "");
        return false;
    }

    struct bx_mode_parse_params params = {
        .initial_mode = 0777u,
        .result_mask = 07777u,
        .max_numeric_mode = 07777u,
        .umask_value = bx_mode_current_umask(),
        .sticky_bit = BX_MKDIR_STICKY_BIT,
        .x_policy = BX_MODE_X_ALWAYS,
        .is_directory = true,
        .apply_umask_when_who_omitted = true,
        .allow_setuid = true,
        .allow_setgid = true,
        .allow_sticky = true,
    };

    if (bx_mode_parse(text, &params, mode_out)) {
        return true;
    }

    bx_diag(diag, "invalid mode '%s'", text);
    return false;
}

static bool bx_mkdir_parse_options(int argc, char** argv, struct bx_mkdir_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'m', 'm', "mode", BX_OPTION_REQUIRED_VALUE},
        {'p', 'p', "parents", BX_OPTION_NO_VALUE},
        {'v', 'v', "verbose", BX_OPTION_NO_VALUE},
        {BX_MKDIR_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_MKDIR_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
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
                if (!bx_mkdir_parse_mode(result.value, &options->mode, diag)) {
                    return false;
                }
                options->mode_set = true;
                break;
            case 'p':
                options->parents = true;
                break;
            case 'v':
                options->verbose = true;
                break;
            case BX_MKDIR_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_MKDIR_OPT_VERSION:
                options->show_version = true;
                return true;
            default:
                return false;
        }
    }
}

static bool bx_mkdir_emit_created(const char* path, struct bx_diag_ctx* diag) {
    if (fprintf(stdout, "mkdir: created directory '%s'\n", path) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_mkdir_chmod_created_dir(const char* path, mode_t mode, struct bx_diag_ctx* diag) {
    int fd = bx_fd_open_nofollow_cloexec(path, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        bx_perror_path(diag, path);
        return false;
    }

    if (bx_fd_fchmod(fd, mode) != 0) {
        bx_perror_path(diag, path);
        bx_fd_cleanup(&fd);
        return false;
    }

    return bx_fd_close(&fd, path, diag);
}

static int bx_mkdir_create_dir(const char* path, mode_t mode, bool explicit_final_mode) {
    if (!explicit_final_mode) {
        return bx_fd_mkdirat(AT_FDCWD, path, mode);
    }

    mode_t old_umask = umask(0u);
    int rc = bx_fd_mkdirat(AT_FDCWD, path, mode | S_IRWXU);
    int saved_errno = errno;
    umask(old_umask);
    errno = saved_errno;
    return rc;
}

static bool bx_mkdir_create_component(const char* path, bool final_component, const struct bx_mkdir_options* options, struct bx_diag_ctx* diag) {
    mode_t create_mode = (options->mode_set && final_component) ? options->mode : 0777u;
    if (bx_mkdir_create_dir(path, create_mode, options->mode_set && final_component) == 0) {
        if (options->mode_set && final_component && !bx_mkdir_chmod_created_dir(path, options->mode, diag)) {
            return false;
        }
        if (options->verbose && !bx_mkdir_emit_created(path, diag)) {
            return false;
        }
        return true;
    }

    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                return true;
            }
            errno = final_component ? EEXIST : ENOTDIR;
        }
    }

    bx_perror_path(diag, path);
    return false;
}

static bool bx_mkdir_parents(const char* path, const struct bx_mkdir_options* options, struct bx_diag_ctx* diag) {
    char* path_copy = xstrdup(path);
    size_t len = strlen(path_copy);
    size_t start = 0;

    if (len == 0) {
        errno = ENOENT;
        bx_perror_path(diag, path);
        free(path_copy);
        return false;
    }

    if (path_copy[0] == '/') {
        start = 1;
    }

    for (size_t i = start; i <= len; i++) {
        if (path_copy[i] != '/' && path_copy[i] != '\0') {
            continue;
        }

        bool final_component = (path_copy[i] == '\0');
        if (final_component && i > start && path_copy[i - 1] == '/') {
            continue;
        }

        char saved = path_copy[i];
        path_copy[i] = '\0';

        if (path_copy[0] != '\0' && !bx_mkdir_create_component(path_copy, final_component, options, diag)) {
            free(path_copy);
            return false;
        }

        path_copy[i] = saved;
    }

    free(path_copy);
    return true;
}

static bool bx_mkdir_one(const char* path, const struct bx_mkdir_options* options, struct bx_diag_ctx* diag) {
    if (options->parents) {
        return bx_mkdir_parents(path, options, diag);
    }

    mode_t create_mode = options->mode_set ? options->mode : 0777u;
    if (bx_mkdir_create_dir(path, create_mode, options->mode_set) != 0) {
        bx_perror_path(diag, path);
        return false;
    }

    if (options->mode_set && !bx_mkdir_chmod_created_dir(path, options->mode, diag)) {
        return false;
    }

    if (options->verbose && !bx_mkdir_emit_created(path, diag)) {
        return false;
    }

    return true;
}

static int bx_mkdir_run(const struct bx_mkdir_options* options, struct bx_diag_ctx* diag) {
    if (options->show_help) {
        bx_mkdir_print_help(stdout, options->progname);
        return 0;
    }
    if (options->show_version) {
        bx_cli_print_version(options->progname);
        return 0;
    }
    if (options->path_count == 0) {
        bx_diag(diag, "missing operand");
        return diag->exit_status;
    }
    for (size_t i = 0; i < options->path_count; i++) {
        (void)bx_mkdir_one(options->paths[i], options, diag);
    }
    if (options->verbose && fflush(stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
    }
    return diag->exit_status;
}

int bx_mkdir_main(int argc, char** argv) {
    struct bx_mkdir_options options = {
        .progname = bx_cli_progname(argc > 0 ? argv[0] : NULL, "mkdir"),
    };
    struct bx_diag_ctx diag = {.progname = options.progname};
    int status;
    if (bx_mkdir_parse_options(argc, argv, &options, &diag))
        status = bx_mkdir_run(&options, &diag);
    else
        status = diag.exit_status != 0 ? diag.exit_status : 1;
    free(options.paths);
    return status;
}
