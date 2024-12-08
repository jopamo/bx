#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

struct bx_mkdir_options {
    const char* progname;
    bool parents;
    bool show_help;
    bool show_version;
};

static const char* bx_mkdir_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "mkdir";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

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

static void bx_mkdir_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_mkdir_parse_options(int argc, char** argv, struct bx_mkdir_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"parents", no_argument, NULL, 'p'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_mkdir_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+p", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'p':
                options->parents = true;
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

static bool bx_mkdir_create_component(const char* path, bool final_component, struct bx_diag_ctx* diag) {
    if (mkdir(path, 0777) == 0) {
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

static bool bx_mkdir_parents(const char* path, struct bx_diag_ctx* diag) {
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

        if (path_copy[0] != '\0' && !bx_mkdir_create_component(path_copy, final_component, diag)) {
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
        return bx_mkdir_parents(path, diag);
    }

    if (mkdir(path, 0777) != 0) {
        bx_perror_path(diag, path);
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
        bx_mkdir_print_version(options.progname);
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

    return diag.exit_status;
}
