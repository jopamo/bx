#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
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
#include "lib/args_common.h"

#ifdef S_ISVTX
#define BX_MKDIR_STICKY_BIT S_ISVTX
#else
#define BX_MKDIR_STICKY_BIT 01000
#endif

struct bx_mkdir_options {
    const char* progname;
    bool mode_set;
    mode_t mode;
    bool parents;
    bool verbose;
    bool show_help;
    bool show_version;
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

static bool bx_mkdir_parse_options(int argc, char** argv, struct bx_mkdir_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"mode", required_argument, NULL, 'm'}, {"parents", no_argument, NULL, 'p'}, {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 1},         {"version", no_argument, NULL, 2},   {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "mkdir");
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "+:m:pv", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'm':
                if (!bx_mkdir_parse_mode(optarg, &options->mode, diag)) {
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
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case ':':
                bx_cli_diag_option_requires_arg(diag, optopt, optind, argc, argv);
                return false;
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

int bx_mkdir_main(int argc, char** argv) {
    struct bx_mkdir_options options;
    struct bx_diag_ctx diag = {
        .progname = "mkdir",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_mkdir_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_mkdir_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    for (int i = first_operand; i < argc; i++) {
        (void)bx_mkdir_one(argv[i], &options, &diag);
    }

    if (options.verbose && fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
