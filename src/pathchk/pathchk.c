#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"

#define BX_PATHCHK_POSIX_PATH_MAX 255L
#define BX_PATHCHK_POSIX_NAME_MAX 14L

#ifdef PATH_MAX
#define BX_PATHCHK_FALLBACK_PATH_MAX ((long)PATH_MAX)
#else
#define BX_PATHCHK_FALLBACK_PATH_MAX 4096L
#endif

#ifdef NAME_MAX
#define BX_PATHCHK_FALLBACK_NAME_MAX ((long)NAME_MAX)
#else
#define BX_PATHCHK_FALLBACK_NAME_MAX 255L
#endif

struct bx_pathchk_options {
    const char* progname;
    bool portability;
    bool posix_safety;
    bool show_help;
    bool show_version;
};

static const char* bx_pathchk_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "pathchk";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_pathchk_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... NAME...\n", progname);
    fprintf(stream, "Diagnose invalid or non-portable file names.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -p, --portability  check for most POSIX systems\n");
    fprintf(stream, "  -P                 check for empty names and leading '-'\n");
    fprintf(stream, "      --help         display this help and exit\n");
    fprintf(stream, "      --version      output version information and exit\n");
}

static void bx_pathchk_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_pathchk_parse_options(int argc, char** argv, struct bx_pathchk_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"portability", no_argument, NULL, 1},
        {"help", no_argument, NULL, 2},
        {"version", no_argument, NULL, 3},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_pathchk_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+pP", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'p':
                options->portability = true;
                break;
            case 'P':
                options->posix_safety = true;
                break;
            case 1:
                options->portability = true;
                options->posix_safety = true;
                break;
            case 2:
                options->show_help = true;
                return true;
            case 3:
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

static long bx_pathchk_query_limit(int selector, long fallback) {
    errno = 0;
    long value = pathconf(".", selector);
    if (value < 0) {
        return fallback;
    }
    if (value <= 0) {
        return fallback;
    }
    return value;
}

static bool bx_pathchk_is_portable_char(unsigned char ch) {
    return ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-');
}

static bool bx_pathchk_check_portable_chars(const char* name, struct bx_diag_ctx* diag) {
    const unsigned char* p = (const unsigned char*)name;

    while (*p != '\0') {
        if (*p != '/' && !bx_pathchk_is_portable_char(*p)) {
            bx_diag(diag, "non-portable character '%c' in file name '%s'", *p, name);
            return false;
        }
        p++;
    }

    return true;
}

static bool bx_pathchk_check_leading_dash(const char* name, struct bx_diag_ctx* diag) {
    const char* p = name;

    while (true) {
        while (*p == '/') {
            p++;
        }

        if (*p == '\0') {
            return true;
        }

        const char* component = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }

        if (component[0] == '-') {
            bx_diag(diag, "leading '-' in a component of file name '%s'", name);
            return false;
        }
    }
}

static bool bx_pathchk_check_component_lengths(const char* name, long name_max, struct bx_diag_ctx* diag) {
    const char* p = name;

    while (true) {
        while (*p == '/') {
            p++;
        }

        if (*p == '\0') {
            return true;
        }

        const char* component = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }

        size_t component_len = (size_t)(p - component);
        if ((long)component_len > name_max) {
            bx_diag(diag, "limit %ld exceeded by length %zu of file name component '%.*s'", name_max, component_len, (int)component_len, component);
            return false;
        }
    }
}

static bool bx_pathchk_check_path_length(const char* name, long path_max, struct bx_diag_ctx* diag) {
    size_t len = strlen(name);
    if ((long)len > path_max) {
        bx_diag(diag, "limit %ld exceeded by length %zu of file name '%s'", path_max, len, name);
        return false;
    }

    return true;
}

static bool bx_pathchk_check_existing_prefixes(const char* name, struct bx_diag_ctx* diag) {
    const char* slash = strchr(name, '/');
    while (slash != NULL) {
        size_t prefix_len = (size_t)(slash - name);
        if (prefix_len > 0) {
            char* prefix = malloc(prefix_len + 1);
            if (prefix == NULL) {
                bx_diag(diag, "memory allocation failed");
                return false;
            }

            memcpy(prefix, name, prefix_len);
            prefix[prefix_len] = '\0';

            struct stat st;
            if (stat(prefix, &st) == 0) {
                if (!S_ISDIR(st.st_mode)) {
                    bx_diag(diag, "%s: Not a directory", name);
                    free(prefix);
                    return false;
                }
            }
            else if (errno != ENOENT) {
                bx_perror_path(diag, name);
                free(prefix);
                return false;
            }

            free(prefix);
        }

        slash = strchr(slash + 1, '/');
    }

    return true;
}

static void bx_pathchk_check_name(const char* name, const struct bx_pathchk_options* options, struct bx_diag_ctx* diag) {
    if (name == NULL || name[0] == '\0') {
        bx_diag(diag, "empty file name");
        return;
    }

    if (options->posix_safety && !bx_pathchk_check_leading_dash(name, diag)) {
        return;
    }

    long path_max = options->portability ? BX_PATHCHK_POSIX_PATH_MAX : bx_pathchk_query_limit(_PC_PATH_MAX, BX_PATHCHK_FALLBACK_PATH_MAX);
    long name_max = options->portability ? BX_PATHCHK_POSIX_NAME_MAX : bx_pathchk_query_limit(_PC_NAME_MAX, BX_PATHCHK_FALLBACK_NAME_MAX);

    if (options->portability && !bx_pathchk_check_portable_chars(name, diag)) {
        return;
    }

    if (!bx_pathchk_check_component_lengths(name, name_max, diag)) {
        return;
    }

    if (!options->portability && !bx_pathchk_check_existing_prefixes(name, diag)) {
        return;
    }

    (void)bx_pathchk_check_path_length(name, path_max, diag);
}

int bx_pathchk_main(int argc, char** argv) {
    struct bx_pathchk_options options;
    struct bx_diag_ctx diag = {
        .progname = "pathchk",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_pathchk_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_pathchk_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_pathchk_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    for (int i = first_operand; i < argc; i++) {
        bx_pathchk_check_name(argv[i], &options, &diag);
    }

    return diag.exit_status;
}
