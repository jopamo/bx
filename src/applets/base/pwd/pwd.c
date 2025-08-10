#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"

enum bx_pwd_mode {
    BX_PWD_MODE_LOGICAL = 0,
    BX_PWD_MODE_PHYSICAL,
};

struct bx_pwd_options {
    const char* progname;
    enum bx_pwd_mode mode;
    bool show_help;
    bool show_version;
};

static const char* bx_pwd_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "pwd";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_pwd_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    fprintf(stream, "Print the full filename of the current working directory.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -L, --logical   use PWD from environment, even if it contains symlinks\n");
    fprintf(stream, "  -P, --physical  resolve all symlinks (default)\n");
    fprintf(stream, "      --help      display this help and exit\n");
    fprintf(stream, "      --version   output version information and exit\n");
}

static void bx_pwd_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_pwd_parse_options(int argc, char** argv, struct bx_pwd_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"logical", no_argument, NULL, 'L'}, {"physical", no_argument, NULL, 'P'}, {"help", no_argument, NULL, 1}, {"version", no_argument, NULL, 2}, {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_pwd_progname((argc > 0) ? argv[0] : NULL);
    options->mode = BX_PWD_MODE_PHYSICAL;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "LP", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'L':
                options->mode = BX_PWD_MODE_LOGICAL;
                break;
            case 'P':
                options->mode = BX_PWD_MODE_PHYSICAL;
                break;
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

static bool bx_pwd_contains_only_normal_components(const char* path) {
    if (path == NULL || path[0] != '/') {
        return false;
    }

    const char* cursor = path;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            cursor++;
        }

        if (*cursor == '\0') {
            break;
        }

        const char* component_start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }

        size_t component_len = (size_t)(cursor - component_start);
        if ((component_len == 1u && component_start[0] == '.') || (component_len == 2u && component_start[0] == '.' && component_start[1] == '.')) {
            return false;
        }
    }

    return true;
}

static bool bx_pwd_logical_path_matches_cwd(const char* logical_path) {
    struct stat cwd_stat;
    struct stat path_stat;

    if (stat(".", &cwd_stat) != 0) {
        return false;
    }

    if (stat(logical_path, &path_stat) != 0) {
        return false;
    }

    return (cwd_stat.st_dev == path_stat.st_dev) && (cwd_stat.st_ino == path_stat.st_ino);
}

static const char* bx_pwd_get_logical_path(void) {
    const char* logical_path = getenv("PWD");
    if (logical_path == NULL) {
        return NULL;
    }

    if (!bx_pwd_contains_only_normal_components(logical_path)) {
        return NULL;
    }

    if (!bx_pwd_logical_path_matches_cwd(logical_path)) {
        return NULL;
    }

    return logical_path;
}

static char* bx_pwd_getcwd_dup(void) {
    size_t size = 128u;
    char* cwd = xmalloc(size);

    while (getcwd(cwd, size) == NULL) {
        if (errno != ERANGE) {
            free(cwd);
            return NULL;
        }

        size *= 2u;
        cwd = xrealloc(cwd, size);
    }

    return cwd;
}

int bx_pwd_main(int argc, char** argv) {
    struct bx_pwd_options options;
    struct bx_diag_ctx diag = {.progname = "pwd", .exit_status = 0};
    int first_operand = 1;

    if (!bx_pwd_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return 1;
    }

    if (options.show_help) {
        bx_pwd_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_pwd_print_version(options.progname);
        return 0;
    }

    if (first_operand < argc) {
        fprintf(stderr, "%s: ignoring non-option arguments\n", options.progname);
    }

    const char* output_path = NULL;
    char* physical_path = NULL;

    if (options.mode == BX_PWD_MODE_LOGICAL) {
        output_path = bx_pwd_get_logical_path();
    }

    if (output_path == NULL) {
        physical_path = bx_pwd_getcwd_dup();
        if (physical_path == NULL) {
            fprintf(stderr, "%s: failed to determine current directory: %s\n", options.progname, strerror(errno));
            return 1;
        }

        output_path = physical_path;
    }

    printf("%s\n", output_path);
    free(physical_path);
    return 0;
}
