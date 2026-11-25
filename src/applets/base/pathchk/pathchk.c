#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/option_scan.h"

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
    bool portability;
    bool posix_safety;
};

static void bx_pathchk_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... NAME...\n", progname);
    fprintf(stream, "Diagnose invalid or non-portable file names.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -p, --portability  check for most POSIX systems\n");
    fprintf(stream, "  -P                 check for empty names and leading '-'\n");
    fprintf(stream, "      --help         display this help and exit\n");
    fprintf(stream, "      --version      output version information and exit\n");
}

enum {
    BX_PATHCHK_OPT_PORTABILITY = 256,
    BX_PATHCHK_OPT_HELP,
    BX_PATHCHK_OPT_VERSION,
};

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
    static const struct bx_option_spec specs[] = {
        {'p', 'p', NULL, BX_OPTION_NO_VALUE},
        {'P', 'P', NULL, BX_OPTION_NO_VALUE},
        {BX_PATHCHK_OPT_PORTABILITY, 0, "portability", BX_OPTION_NO_VALUE},
        {BX_PATHCHK_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_PATHCHK_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };
    struct bx_pathchk_options options = {0};
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "pathchk"),
    };
    const char** names = argc > 1 ? calloc((size_t)(argc - 1), sizeof(*names)) : NULL;
    if (argc > 1 && names == NULL) {
        bx_diag(&diag, "memory exhausted");
        return diag.exit_status;
    }
    size_t count = 0;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc,
                          BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            names[count++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            bx_cli_print_try_help(diag.progname);
            goto done;
        }
        switch (option.spec->id) {
            case 'p': options.portability = true; break;
            case 'P': options.posix_safety = true; break;
            case BX_PATHCHK_OPT_PORTABILITY:
                options.portability = true;
                options.posix_safety = true;
                break;
            case BX_PATHCHK_OPT_HELP:
                bx_pathchk_print_help(stdout, diag.progname);
                goto done;
            case BX_PATHCHK_OPT_VERSION:
                bx_cli_print_version(diag.progname);
                goto done;
        }
    }

    if (count == 0) {
        bx_diag(&diag, "missing operand");
        bx_cli_print_try_help(diag.progname);
        goto done;
    }

    for (size_t i = 0; i < count; i++)
        bx_pathchk_check_name(names[i], &options, &diag);

done:
    free(names);
    return diag.exit_status;
}
